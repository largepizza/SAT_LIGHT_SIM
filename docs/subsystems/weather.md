# Weather objects, lightning, weather evolution

Storm objects (towers, shields, squall lines, MCSs, cyclones, tornadoes, surge), biome map + wildfires, haboobs, the Weather window, lightning + thunder, weather-cube evolution.

Part of the subsystem notes (`docs/subsystems/`, map in `AGENTS.md` §4). Moved verbatim from
`CLAUDE.md` on 2026-11-03. Do not read this file whole: `python tools/outline.py docs/subsystems/weather.md`
lists its sections and topics with line numbers. New notes go next to the section they belong to.

## Storm objects, wildfires, haboobs and the Weather window (from "Subsystem: Clouds v2")

- **Storm OBJECTS (2026-10-22/23, branch `weather`, `.plans/WEATHER_EVENTS_PLAN.md`):** the towers are no longer a
  lattice over the map's Cb regions (continent-sized alleys of anvils) but storms spawned by `wx::Catalog`
  (`Weather.h/.cpp`): hashed per (equal-area cell, 30-min bin) at a flash-density climatology (`StormClimatology`,
  analytic until the LIS/OTD table is baked) x the map's moisture, x5 per flash at sea; kinds air-mass / multicell /
  supercell with their whole lifecycle fixed at birth, so every storm is a pure function of sim time (warp, reverse,
  bookmarks). They live in the MAP frame (cv2Drift) and ride with it. Each frame `updateStorms` (SatelliteSimWeather.cpp)
  uploads the towers alive (`GpuStormTower` = `CV2Tower`, 48 B; strength = the lifecycle, a weaker tower is SHORTER)
  and an open-addressed hash of 256-per-face cube bins (each tower listed in every bin its reach touches — only those, since
  2026-10-27: `clouds_v2.storm_index_tight`, see "Weather perf pass" under GPU Performance Profiling) into
  `stormBuf` (device-local host-visible): march binding 26, beam occlusion 10, lightning, cdc. `cv2ColumnSigma` loops
  over its point's bin; lightning flashes only from them. Settings `clouds_v2.storm_objects / storm_frequency /
  storm_life` (Weather tab "Storm objects"); harness `storms`, `storms go`, `state` -> `weather`; `tools/weather_tool`.
  **A storm brings its own cloud into the weather cube** (2026-10-23, issue 20261022_233857: a supercell in near-clear
  map air rained from nothing under its anvil; from above, anvils on bare ground). `cloud_v2_weather.comp`
  `stormStamp` (binding 4 = `stormBuf`): within each tower's ENVELOPE (`cv2StormEnvRadius` = reach + max(40 km, 0.8
  reach)) a broken cumulus field thickening toward the tower (congestus within "Storm cumulus reach"), a stratiform
  raining deck under the anvil's forward flank (type ~0.15: the low layer thickens it), rain there, and a clear moat;
  "Storm cloud field" `storm_field` 1 / "Storm clear moat" `storm_moat` 1 (slots 323-324).
  **The moat** (2026-11-10, issue_20261110_185127 "Storm moats", worktree storms): a ring from the edge of the storm's
  cumulus (Rf = 1.15 Rc + max("Storm cumulus reach", 0.5 Rc)) out "Storm moat width" `storm_moat_width_km` (12),
  its outer edge feathered over "Storm moat feathering" `storm_moat_feather` (0.6) of the width (>= 1.5 km), round
  the share "Storm moat frequency" `storm_moat_frequency` (0.5) of the storms (the DOMINANT tower's seed hashed:
  role > 0.9, so one draw per storm; slots 568-570), x "Storm clear moat". Such a tower's reach (`SatelliteSimWeather.cpp`,
  the GPU tower's c.x; not a cyclone's) covers the ring, or the march's walk stopped at the old reach and every width
  looked the same. (Gating the moat on a spreading anvil, anvilK x "Cb flare" > 1.02-1.15, removed it from every storm of
  the report: tried and dropped.) The same ring in two places: the bake lowers
  the low field's coverage over it, and the march's mid layer gives way over it (`cv2StormGeom`'s `storm`, which
  `cv2Field` cuts the mid layer by). The bake's push constants are full (128 bytes): width, feathering and frequency
  ride packed in the high parts of `storm.y`, `storm.w`, `storm2.y` (`SatelliteSim::weatherEvoPC`). The RINGS of the
  report were not this moat (it was 0): the mid layer gave way out to 0.6-1.0 of each tower's whole reach (its anvil's,
  ~50-80 km), and from orbit at a low Sun every storm stood in a dark annulus of the low field's shadowed cumulus where
  the orange mid deck had been cut (knockout: `mid_amount` 0 removes the rings with the deck; `storm_field` 0 and
  `cb_cumulus_reach_km` 0 leave them). The mid layer now gives way only over the towers (1.15 Rc), and past them only
  where a storm has a moat.
  Invariants: (1) the bake reads a SECOND index (envelope bins + its own tower copy, `kStormEnv*`, declared only under
  `CV2_STORM_ENV`) that the march never reads: listing towers to their envelope in the march's index would multiply
  the column search. Built only on a frame the bake runs (`buildStormEnvIndex`, ~1-2 ms; every frame it cost +2 ms).
  (2) The low cloud reads the cube through the FLOW (`cv2FlowDisp`, up to hundreds of km), so the envelope copy's axis
  is the tower's FLOWED position (a double CPU replica of cv2FlowDisp: keep it in step), or the field lands beside the
  storm. (3) The cube re-bakes a face at most 45 s behind with storms stamped, and every face after a jump of > 15 min.
  The rain core under a dominant tower is its body (it was an ellipse 2.5x longer downwind, under the anvil, where
  there is no cloud below). Cost: within noise at 1 h/s (bake), 128 registers.
  **Drawn through the cumulus field (2026-11-04, `clouds_v2.storm_unified` 1; clouds.md "Storms through the cumulus
  field"):** the towers and the shield's plate are a margin `cv2StormGeom` hands the low layer, carved by its billow lobes
  and erosion; everything below (catalog, strengths, heightCap, spread, shield age, rain core, lightning, the weather
  stamp) feeds that geometry unchanged. The shield's thin outer veil stays cv2AnvilSigmaP's, as thin ice.
  **Anvil SHIELDS (2026-10-23, issues 20261023_002905 / _001522):** every storm has one (`wx::Shield`, `Catalog::shieldsAt`;
  `CV2Shield` 64 B + its own index of 128-per-face bins (64 until 2026-10-27; the count is `cv2ShieldHead.y`; the bake's
  envelope copy keeps 64), `cv2ShieldAt`, between the tower list and the env block of
  `stormBuf`): a capsule from 0.3 rA upwind of the anvil-reaching towers' heads (strength-weighted) to L downwind (L grows
  at 0.6 x the storm's `upperWind` to 45 / 90 / 160 km by kind), radius rA (the head) growing to 1.15 rA + 0.25 L. Its
  strength rises over the "Anvil formation time" after a tower first passes 0.42 (`TowerSpec::anvOn`, precomputed at
  birth) and LINGERS `Storm::linger` (70-240 min by kind at the default "Anvil decay time", drawn after the towers' Rng
  draws so every storm's towers stayed as they were) after the last falls below it, drifting at 0.8 x upperWind, thinning (thick x 0.4, density x 0.5) and fraying (`age01`): orphan
  anvils turning into cirrus. `Storm::life` includes the linger (`kMaxLifeS` 8 h); the spawn rate is calibrated on the
  towers' life alone, so storm counts are unchanged. `cv2AnvilSigmaP` reads the shields instead of the map's cumulonimbus
  typing when storm objects are on (`cv2ShieldsOn`: `cv2ShieldHead.y`): fed by the map, shields stood over regions with no
  tower ("stranded anvils"). Its outline is frayed by the cluster fBm (x6-7.5: that field only spreads +-0.06) and the
  storm lobes at 1x and 3x, the factor floored at 0.8 so the outline never passes the 1.6 radii the shield is binned to (the
  capsule's clean profile drew white pills from orbit), and `presA` is 0 at its 0.05 cutoff (it kept 2% there: at Anvils 8
  a view along the edge drew a hard arc). The user's anvil tune is the default (Anvils 8, thickness 3.7 km, hang 0.87 km,
  puffiness 0.45, lift 230 m, head lobes 0.12, lobes 0.37, waist 0.89, overshoot 0.14 km, storm erosion 0.24).
  **Storms billowy, not pitted (2026-11-08, the user: intense weather "extremely spikey and eroded ... more intensive storms
  are more billowy and continuous"):** "Storm cumulus fine lobes" 0.5 -> 0, "Storm bubbles" 1 -> 0.5, "Storm erosion"
  (share of the erosion kept on deep convection) 0.24 -> 0.04: the towers' surface is the storm-scale lobes' big billows
  instead of a pitted crust of cumulus-scale lobes, bubbles and erosion (ITCZ report 184711; 0.57 ms faster there). Tried
  first: the storm share's profile round the towers (stormProx into sK: smooth vertical slabs, the coarse cells extruded)
  and more cumulus top headroom near storms (no visible change).
  **Hurricanes as one storm mass (2026-11-08, branch `weather-salvage`; weather-v2's "removal of the top layer and insistence
  on just modifying the noise", which the user found "tremendously better"):** the CDO is no plate shield
  (`Catalog::cycloneShieldsAt` emits only the cirrus deck). The plate and everything only it read were RETIRED the same day
  (the user: "we do not need to maintain things like the old plate"): the CDO swirl texture in cv2ShieldAtR (a shape-volume
  read), the top smoothing in cv2FieldLow, the swirl ridges on its margin, "Hurricane top smoothing / streaks / density"
  (slots 522-524, ciHu.zw, ciHu2.x now 0) and the brief "Hurricane CDO plate" (547). Every view of the baseline 0.9-2.6 ms
  faster (code size; the march stays at 255 registers), report 172424 shot 2 -8 ms (harness_runs/salv_after3 vs _after2). (1) The bake types the
  CDO 0.95 (cloud_v2_weather.comp, its own share: gCycDeck carries "Storm cloud field", which at the user's 0.65 left the type
  at ~0.7). (2) Where the type passes congestus the low field draws ONE storm mass (`sK`, "Storm mass" `storm_mass` 1 =
  cv2.column.y, slot 548; ported from weather-v2's continuous storms): tops to 0.8 of the span from the tower noise (cluster
  Perlin at 1/16 and 1/4 period + coarse cells), shoulders 0.15-0.28, strong cores to the top (capS), no cumulus headroom,
  cells a mip coarser, lobes x (1 + 2 sK); the far column takes full cover and strength. Erosion stays the storm's (v2's
  full cumulus erosion read "spiky and eroded"). (3) Its top is an ice SHEET in the high layer (cv2HighSigma, v2's anvil
  sheet): just under the tropopause, 400 m + "Anvil thickness", lumpy below and above, dense (0.012), lit as cloud
  (gCv2AnvK off f.thin); its share is ANALYTIC from the cyclone list (`cv2CdoShare`: out to 0.7-1.1 CDO radii warped by
  the regime noise and the cluster Perlin, open at 0.9-1.5 eye radii, x intensity) -- read from the map's type it was a
  blocky slab with no eye. GPU: report 172424 shot 2 37.6 ms, from 350 km 47.2 (plate: ~39 / 49). March 255 registers.
  **The CDO filled, its eyewall and its sheet's edge (2026-11-08, issue_20261108_173906):** (1) THE MASS REACHES ITS TOP.
  With the Cb columns on, cv2LowA's storm-target block (`if (colOn)`) capped every Cb-typed region at the cumulus top
  (~4 km) away from a tower and at "Storm cumulus top" (7 km at the reporter's settings) within reach of one, AFTER the
  storm share had set the tropopause, and the early out `stormProx <= 0 && q.h > hTypB` (the CPU's anvil3.zw bound, which
  has no storm mass in it) clipped it at ~5 km: the CDO's interior was flat mesas at 4-7 km under an empty 7 km to the
  sheet. Now the storm share re-takes its own top after that block, and near a cyclone's CDO (1.6 CDO radii + 50 km of
  `cv2Cyclones[i].a`, looked up only for samples above the bound) the bound is the Cb top + 4 km. The mass's towers
  now reach their cap, 0.8 +- 0.1 of the span (the 16-km cluster octave and the coarse cells, met by a smooth min; a hard
  0.8 drew one flat plain under the sheet), ~11-14 km: the report's shot 2 eye (12 km) is inside cloud, from 8 km too.
  (2) THE EYEWALL is the storm mass, not the 18 tower objects: within 0.5-1.2 radii of maximum wind of the eye's edge
  (`eyw`, x the eye's grown share `smoothstep(0.2, 0.6, eyeR / 0.45 rmw)`, x "Hurricane eyewall" hurricane.y, slot 557) the
  span rises 4 km and every cell is a tower (cellsC + 1.5 eyw: the tower noise saturates); the eye is cut in 3D as a
  STADIUM through covT (1 unit of coverage per 2.5 km inside it), radius 0.85 eyeR at the base to 1.2 at the top, x (1 +-
  0.22) of the cell and cluster noise plus a height wave (`sin(h / 1.7 km + 40 ce.a)`): fluted, bulging walls, stretched
  cumulus, sloping out as the real eye does. The sheet's hole opens over 1.1-1.6 eyeR so the wall's top stands inside it.
  The ring's tower objects (`Weather.cpp cycloneTowersAt`, role 1) fade out of the march's copy by the same share
  (`SatelliteSimWeather.cpp`, the GPU strength x (1 - k), unlisted at k >= 0.98); the bake's envelope copy keeps them,
  so their rain, stamp and lightning stay. Their columns stood in the wall as separate cylinders with flat heads.
  (3) THE SHEET'S EDGE AND SWIRL ("Hurricane sheet swirl" hurricane.x, slot 556; `cv2CdoShare`): on the bake's own main
  arms (same phase as stormStampFinish) the outline reaches out along each crest (u x 0.75, to ~1.45 CDO radii) and is
  notched between them (x 1.3); its outer ~40% frays into 3n + 1 streaks wound tighter (lr / 0.6 tanP), their phase moved
  by the outline's noise (breaks); the top rises and falls 140 m along a 12-band spiral (lr / 0.45) with the noise in its
  phase (`gCv2CdoRip`). From orbit the CDO is a spiral with arms and frays, no longer a round disc.
  **The low field's ramp, its smoothing and the eye's bowl (2026-11-10, hurrshape; issues 20261109_184510 "Hurricane low
  clouds", 20261109_184650 "Odd cloud shapes in hurricane eye", 20261105_232743 shot 2; the user: "turn down erosion on hurricane
  cumulus clouds; make the lower part fuller and smoother, rising towards the eye wall, where it meets the upper disk").**
  Attribution at 184510 (11.9 km, ~3 RMW out under the CDO): the canyons and fluted columns were the storm mass's tops from
  its 2D tower noise (shoulders 0.15-0.28 of the span, towers to 0.8): every column a cell outline extruded, with clear air
  from ~4-8 km up to the sheet; "Storm erosion" (0.04 there), "Storm puffiness" / "tower height" / "fullness" and the
  cumulus lobes did not change it, "Storm mass" 0 left a flat low deck under an empty gap. Now (cv2LowA, "THE HURRICANE'S
  RAMP", one pass through the cyclone list shared with the eyewall): (1) hR = 1 from the eyewall to one RMW past the eye's
  edge, 0 at 0.9 CDO radii; the mass's top T is the ramp 0.35 -> 0.97 of the span (sqrt(hR)) with the tower noise kept
  above it at 1 - "Hurricane smoothing", and the coverage closes (+smoothstep(0, 0.4, hR)): one deep cloud rising to the
  sheet's base at the eyewall ("Hurricane fill" `clouds_v2.hurricane_fill` 1, hurricane.z, slot 600). (2) Within 1.3 CDO
  radii (hW) "Hurricane smoothing" (`clouds_v2.hurricane_smoothing` 0.7, hurricane.w, slot 601) takes the lobes x (1 - 0.6
  k), the erosion x (1 - 0.8 k) and the shallow cut-through x (1 - k): the outer bands' cumulus are calmer too; outside a
  cyclone's reach nothing changes (a fair-weather view on/off differs 1.24 levels mean, its own repeat 1.41). (3) THE EYE
  AS A BOWL: the bake clears the map in 2D only over 0.45-0.8 eye radii (was 0.7-1.15: the wall was that 2D edge
  extruded, the "slabs"); the low field clears any cloud inside the eye radius through covT (1 unit per 1.5 km, gentle
  +-12% noise, whatever "Hurricane eyewall"), and the wall is the mass's top capped by hE = smoothstep(0.7, 1.5 eye radii):
  ~0.16 of the span at the eye's edge rising to the full top, a stadium. The old cut (radius 0.85-1.2 eyeR with height,
  +-0.22 from saturated noise and a 1.7-km height wave) drew terraced shelves and vertical slabs. Measured (RTX 3070 Ti,
  1920x1009, interleaved base/new march + cdc SPVs, 3 reps, 255 registers both): 184510 shot 1 GPU 29.5 -> 18.9 ms (cloud
  march 22.3 -> 11.0: the filled column goes opaque in a few steps), 184801 (the eye from above) 56.8 -> 46.0 (47.7 ->
  37.7), 172424 shot 7 (orbit) 37.8 -> 37.4. High-pass energy of the low cloud (|I - blur3|, lower 2/3 of the frame) at
  184510 shots 1 / 2: -42% / -57%. From inside the CDO below ~13 km a camera is now inside cloud (a real CDO is).
  **The cyclone flow map (2026-11-10, flowmaps; issue_20261109_184801 "Hurricane rotation", 20261106_172424 shot 7, 20261105_232743
  shots 1 and 3; `clouds_v2.glsl cv2CycFlowAt`, `cloud_v2_resolve.comp`, `SatelliteSimWeather.cpp`):** the cloud TEXTURE round a
  tropical cyclone turns about its centre and rides with the storm, while the weather cube's stamp (the bands, the deck, the
  eye: the storm's envelope, quasi-stationary in its frame) stays put and the texture streams through it, as the cloud
  elements of a real rainband stream along the band. `cv2Field` evaluates the displacement ONCE per sample and folds it into
  `q.rSeaE` / `q.seaProjE` (undrifted), so every layer's noise read (cells, cluster, shape, storm and detail volumes, the mid
  layer's, the sheet's lumps) takes it with no state of its own: held as a global across the layers, with the loop inlined
  at five read sites, the march compiled to 255 registers (+13-23 ms); folded into q it is 128, as the base (binary +7 KB,
  spill +16 B). (1) The rate: a modified Rankine vortex, vmax / rmw x "Hurricane rotation" (`clouds_v2.hurricane_rotation`,
  0.6, slot 560) inside the radius of maximum wind, x (r / rmw)^-1.5 outside, to 0 over the outer half of the bands' reach;
  cyclonic (counter-clockwise in the north). (2) A rotation by rate x time winds up without bound, so it is a flow map like
  the cirrus fibres' (`cv2CirrusFlow`): two phases half a window apart, each a rotation by the rate x its own age, renewed
  in turn; the window = "Hurricane flow winding" (`hurricane_flow_turns`, 0.25 turn, slot 561) over the rate at the RMW,
  clamped 5 min .. 2 h, its clock integrated per cyclone on the CPU (`cycFlowPhase`, `CV2Cyclone.e.x`: the record grew a
  fifth row, 80 B) and the renewal staggered by a smooth field of (angle, ln r) in the storm's frame so no ring or disc
  renews at once. Within the blend (`hurricane_flow_blend`, 0.12 of the window each side, slot 562) the phases are DITHERED
  per ray (the pixel-and-frame hash `cv2ShapeUseB` uses) and the resolve's history averages the rays into the cross-fade: a
  density blend costs a second field evaluation at every sample in the band, and a blend of the two ANGLES turns the
  texture backward through every renewal; and the winding per window is small and the blend narrow because two phases
  half a window apart are two copies of the texture turned by half the winding -- dithered together, a double exposure:
  at 0.75 turn / 0.2 a sector of the CDO's top was smeared along arcs in every settled view (the first cut). The occupancy clipmap's bake picks the phase per bake POINT (`CV2_FLOW_POINT_DITHER`)
  so a cell covers both; the other bakes take the dominant phase. (3) The STORM LOCK: the noise frame drifts with the map
  (`clouds.drift_rate`) and the storm follows its own track through it, so the clouds slid through every storm at ~25 m/s
  (232743 shot 3); within the reach the texture is dragged with the centre (`e.zw`: Rz(drift) x its track velocity + the
  drift under it, east / north at the centre), as far as the fade band's shear stays under ~0.5 (`lockK`), plus a jitter per
  renewal (<= 0.12 CDO radii) so a stationary storm does not show the same unwound clouds every window. (4) The resolve
  reprojects the rate's and the lock's motion for the cyclone the eye is deepest in (`cv2.flowA` / `flowB` / `flowC`,
  `fillCloudsV2Params`): the point that shows this cloud now showed it turned back and shifted back last frame. (5) The
  sheet's ripple turns with the content (`cv2CdoShare` recomputes the turn locally: a few flops on sheet samples, not a
  register the march pays for), and its lumps (`lmp`, now read before the sheet block) raise and lower the top by 400 m x
  their value so the turn shows from above, where the thick sheet's density does not; the lumps' period follows the CDO's
  radius (x0.7 .. 1.6 over 100 .. 400 km) with a second read at a third of it (232743 shot 1 "repetitive, size is not a
  factor"). (6) Two causes of "the arms teleport" that were not the rotation: the bake read the arms' ragged texture
  (`tex`) at the map direction, so the storm moved through it at ~25 m/s and every outline was re-cut by unrelated 5-40 km
  cells (now read at the centre + the offset: it travels with the storm); and with "Hurricane main arms" at 0 (auto) the
  count followed the moment's vmax, and `floor(arms + 0.15)` stepping as a storm weakened re-placed every arm-cirrus
  capsule and band tower in one frame (at 827 km, 1 h/s: arm-sized patches switching at the same sim minutes with the
  rotation off, the clipmap off and the cube's rect bake off) -- the auto count now follows the storm's PEAK wind. Perf
  (RTX 3070 Ti, 1920x1080, A B interleaved in one app alone on the GPU, 3 reps of 60 frames, no pre-waits; the first
  A/B on a shared GPU read +0.5-0.7 ms and was wrong): cloud march +4.6 and +4.1 ms over the eye from 46 km (184801,
  47.6 -> 52.3, 47.2 -> 51.2), +2.8 and +9.9 ms over the CDO from 61 km (232743 shot 1; the base itself moved 58.3 -> 62.7
  between the runs: the post-reload regime), +0.8 and +1.1 ms with no cyclone in view (125245 from 650 km); 128 registers on
  every reload. OVER the wave's +0.5 ms budget: the per-sample loop (a cross, a length, three sines, sin / cos, the hash)
  runs at every field sample inside a reach, and in a hurricane view that is every sample; "Hurricane rotation" 0 returns
  it to the base's cost (the loop's first test). A cheaper form (the stagger without atan / log, inversesqrt for the
  profile, the lock share on the CPU) did not move the number: the remaining cost is the loop's existence per sample. The
  next step would be one evaluation per ray or per coarse step (the displacement varies over ~10 km), not per sample.
  The artifacts gate (`artifacts.py`, hurr_eye / hurr_orbit / hurr_top, 1x / 1m / 5m; the after run under the 2-second
  settle cap, so its lag reference is not converged): the eye view's lag16 6.9 -> 10.8 % (1x), 9.7 -> 11.6 (1m), 9.5 ->
  11.3 (5m) and grain 1.0 -> 1.2 (the eyewall turns 25 deg a frame at 5 min/s, which the sparse-block history cannot
  hide); the orbit view's lag16 0.75 -> 0.56 (1m), 1.7 -> 0.95 (5m); the top view's lag16 0.9 -> 0.02 (1x). The 827-km
  1 h/s change curve: spikes at 5.9 / 4.8 / 5.3 x the median on the base (frames 18, 78, 212) -> one at 3.5 x (frame 78).
  Measured turn (250 km straight down, 5 min/s, polar cross-correlation at 1.1-1.6 RMW): 5-8 deg counter-clockwise per
  5 min (the rate predicts ~16; the history smears the rest); at 4-6 RMW the sheet is featureless under a noon Sun, so
  nothing can be tracked there. **Round 2 (the same day):** the march computes the displacement ONCE at its main sample
  (`CV2_CYC_EXTERNAL`, as `CV2_ORO_EXTERNAL`) and the light march's samples of that step reuse it (it varies over ~10 km;
  the light steps are within a few km): the loop ran in every inlined cv2Field, 12 light samples per main sample. The CPU
  counts the cyclones whose reach meets the view's cap (`flowP.w`; the eye's horizon + 6 deg): none, and the march does
  one uniform test per sample. THE TIME-LAPSE FADE: past ~2 deg of turn a frame at the RMW (1 h/s gives ~10; 5 min/s 0.8)
  the rate fades to 0 over 2-8 deg, smoothed over ~10 frames (`cv2FlowFadeS`): a turn the frame rate cannot resolve is
  not shown, the storm lock stays; the resolve's window keeps the unfaded rate so its lock share matches the upload's.
  A ray runs the loop only when a vortex's reach comes near its shell segment (`cycNear`, the two ends' map directions
  against the reach + the segment's span), and the displacement is refreshed every 1 km of the ray. SUPERCELLS: each
  supercell's dominant tower is appended to the cyclone records as a MESOCYCLONE (`cv2CycHead.y` counts both; the
  hurricane consumers read `.x`): the same vortex on a 2-3 km RMW at 20-35 m/s (~a turn in 6-10 min), reach 5 RMW, no
  storm lock, the nearest to the eye in the records left; the wall cloud and the base turn under the updraft.
  ROUND-2 NUMBERS (A B interleaved alone on the GPU, 3 x 60 frames, no pre-waits): no vortex in reach ~0 (650 km orbit
  with a cyclone inside the view's cap 28.06 -> 28.03 ms; a storm-free ground view 14.38 -> 14.34); the tornado view of
  issue_20261108_173628 (a supercell 5 km off) 28.30 -> 29.10 (+0.8); the hurricane views are unchanged by all of it --
  eye from 46 km +3.4 then +3.8 ms, CDO from 61 km +4.3 then +6.0 (its base drifts 58-67 ms between runs) -- so the
  per-sample loop was never their cost; what is left is the field reading two textures a quarter turn apart in
  neighbouring rays (the per-ray dither) -- NOT that either: at the eye view, base 46.5, the per-ray dither 49.7, the
  dominant phase alone 50.5, a 16-px-tile dither 49.5 ms (3 x 60 frames each, 128 registers). The ~+3 ms is the
  displaced reads themselves (every layer's noise read at a position turned about the eye); unexplained beyond that:
  the base and the flow variant differ only in cv2CycFlowAt's body and the displacement it returns. The gate twice on the same exe and settle cap (rotation 0 as
  the base; hurr_eye): lag16 12.2 -> 10.9 % (1x), 12.7 -> 14.0 (1m), 11.8 -> 12.2 (5m), grain equal (0.97 / 0.94 / 0.87).
  The supercell's 1x sequence (3 captures 1 s apart at 173628) shows no measurable turn from that vantage: the funnel is
  5 km off and the wall cloud is not in the frame; ~1 deg of turn at the RMW in 2 s. **Round 3 (2026-11-11, on the
  merged tree 6d4f2d6):** (1) a tree rebuilt after the wave's merges died silently at pipeline creation (after "terrain
  clipmap: supported") twice, even with a 900 s boot timeout: the 238 MB `pipeline_cache.bin` of the pre-merge tree;
  deleted, it boots (the UBO mirror was fine). (2) THE REGISTER CLIFF: on the merged tree the march is at 255 registers
  (hurrshape-a saw it too; every cluster measured 128 alone). One-feature-out variants: the flow map out 255, rain-lit off
  255, grain's stride out 255, storms' `stormIn` out 255, CV2_CYC_EXTERNAL off 255, optics' ice halos out 254, hurrshape's
  fill block out 255; pairs: flow + optics 253, flow + fill 255, flow + storms + grain 255; ALL THREE of flow + fill + ice
  halos out: 128. A sum of three features, no single tipping one. The flow map's own trims did not move it: its
  displacement cleared before the light march (as `gCv2Oro`) 255, that plus the 1-km refresh dropped 254 -- so the clear
  is not kept (the light samples keep the displaced field), and the way back to 128 needs the fill block and the ice
  halos to shed as well (the orchestrator's call across clusters). In the 255 regime the eye view's cloud march is 69 ms (37.7 at 128 per the orchestrator), so the
  cliff is worth ~30 ms: more than every feature it holds. (3) Re-baselined rotation A/B on 6d4f2d6 (by setting, 0 vs 0.6,
  interleaved 3 x 60, no pre-waits, in the 255 regime): eye from 46 km 69.2 -> 71.1 ms (+1.8), CDO from 61 km 82.1 -> 86.4
  (+4.3), the tornado view 23.0 -> 24.6 (+1.6). (4) The tornado funnel already turns (the tornado work: `seedK` carries a
  spin phase, the funnel ~35 m/s at its radius, the wall cloud a turn in ~5 min, debris slower, `cloud_march.comp`
  `tornadoCyl`); round 3 integrates that phase per frame and per part with the time-lapse fade (`tornadoSpin`,
  past 4-12 deg of turn a frame; at 1 min/s the funnel turned 14-69 deg a frame). Measured at 1x, 1.5 km south of the
  funnel of issue_20261108_173628 (r 98 m): the funnel's texture advances +2 px per 0.5 s across it, three captures in a
  row (corr 0.99) -- consistent, slow at that range; the wall cloud from 4 km: 0.01 % of pixels change in 2 s (a turn in
  5 min is 2.4 deg: not visible). **Round 4 (branch `qa/20261110/regdiet` on 11e9478, the register diet):** the cheap
  lever first: `shaders reload wg=32x16` (512 threads a group) caps a thread at 128 registers with the SAME SPIR-V, so
  the two regimes were measured on one build (interleaved, 3 x 60 frames, no pre-waits, alone): registers 255 -> 128,
  spill 1808 -> 2176 B; cloud march at 400 km over the Pacific 28.6 -> 31.0 ms, cumulus from 1.5 km 12.0 -> 17.3, a 4-km
  field 9.7 -> 13.5 -- SLOWER by 2.4-5.3 ms at every ordinary view -- and 69.2 -> 59.7 (-9.5) over the eye from 46 km. So
  the merged march's 255 regime is not the 30-ms loss it looked like: a forced 128 spills more than the wider register
  file costs, except where the eye view's heaviest rays run; the orchestrator's 37.7 ms at the eye on a natural 128 (hurrshape
  alone) against 69.2 here is the merged features' own cost (fill, ice halos, the rotation) plus ~10 ms of regime. The
  workgroup cap is therefore not shipped (`cv2MarchWg` stays 16x16); a natural 128 needs the code diet (the ice halos once
  per ray, the fill ramp without new live state, the displacement not live across the light march) -- not done in this
  round. THE PIPELINE CACHE (`VulkanContext.cpp`): it held every build's pipelines and was discarded only past 256 MB, so
  after the wave's merges it stood at 238 MB of stale entries and the app died silently at pipeline creation with it
  loaded; it is now keyed to a hash of every shipped `shaders/*.spv` (`pipeline_cache.key`, written on save) and starts
  over when any shader changes (one slow launch: "Pipeline cache: starting over (the shaders changed)"), cap 160 MB; a
  fresh cache after a full boot is 11-14 MB. Not shipped: a general flow of all cloud noise on `wfWind` (weather-v2's
  `cv2NoiseDispAt`): it needs the wind evaluated per sample in the march and per pixel in the resolve, and the lattice
  seams it showed from orbit (e453372); the storm lock covers the user's case (storms vs the drift).
  **Anvils carried by the organised systems (2026-11-08, branch `weather-salvage`, `.plans/WEATHER_SALVAGE_PLAN.md`; the
  user: "spawn big anvils and force the objects to stick around. Do not spawn too many anvils near one another. Let anvils
  live for hours"):** (1) A single cell spreads a shield only as a supercell (`wx::Settings::singleAnvils`, "Single-storm
  anvils" `clouds_v2.storm_single_anvils` 1; 0 none, 2 every kind as before; slot 544): every air-mass and multicell storm
  that reached the lid had its own, lingering for hours, ~930 shields worldwide at the user's settings (Plains, July 21Z),
  now ~115 (34 systems + the supercells); the grey, speckled lingering discs from orbit are gone. (2) ONE envelope per
  storm (`Catalog::shieldsAt`): it forms once from the first tower at the lid over min("Anvil formation time", 0.7 x the
  span its towers feed it, >= 10 min), holds while any feeds it and lingers from the last one's fall. Each tower ran its
  own ramp before (a multicell's next tower restarted the spread) and at the user's 240-min formation time no shield
  passed ~0.1 before its linger began. A supercell's shield now forms over ~75 min, holds ~2 h at 1, and is ~0.7 three
  hours on. (3) Squall lines / MCSs are SPACED: one per 6-degree line cell and bin, none for "Storm complex spacing"
  (`storm_complex_gap` 2 bins = 6 h, slot 545) after one (the earlier bins' Poisson draws recomputed: pure in time), placed
  in the middle 60% of the cell; their rate takes "Storm complexes" alone (not "Storm frequency", which thins single
  cells), and over tropical seas they are more often MCS cloud clusters (+0.35 share) than lines. (4) The plate is
  continuous: its lobes only bulge (`lobeRef = -sg.deck`) and "Anvil lid / underside lobes" apply over the whole plate.
  Both were x "Anvil fibres" (plK): at the user's 0.47 half the lobes cut down into a ~700-m plate (a holed, grey sheet
  from 30 km) and half the lid kept the polished 0.4 of the storm lobes (bulges whose shadows mottled it). Lid lobes
  default 0.1 (0.25 still mottles from 80 km at low Sun). (5) An orphan plate recedes as it ages (presence x 1 -
  smoothstep(0.15, 0.6, age)); the thin veil (cv2AnvilSigmaP) lingers.
  **Anvil formation and decay times (2026-11-03, issue 20261030_210137 "anvils pop in and out" at 5 min/s):** two
  settings, "Anvil formation time (min)" `clouds_v2.storm_anvil_form_min` 60 and "Anvil decay time (min)"
  `storm_anvil_decay_min` 70 (Weather tab, Storm objects, slots 450-451; `wx::Settings::anvilFormS` / `anvilLingerS`).
  Formation = how long the single-cell kinds' shield takes to reach full strength from `anvOn` (its radius maturing over
  max(45 min, it)); decay = an air-mass storm's shortest linger (each storm draws 1-2x it, multicells x 45/35,
  supercells x 60/35, capped by `kMaxLifeS`; both are in `stormEnvHash`, so a change rebuilds the catalog). Line
  systems keep their own hours-long ramps. Rise and decay are the RAMP g (0.6 + 0.4 g), not a smoothstep: the drawn
  presence is smoothstep(0.05, 0.6, strength) (`cv2AnvilSigmaP`), so a smoothstep strength put the visible change into
  a quarter of the ramp; this one is ~linear through 0.05..0.6 (10 -> 90% presence over 0.385 of it). The TOWER's
  flared head and the anvil plate under it (`cv2ColumnSigma`'s `near`) were the bigger pop: they switch on with `full`
  (strength 0.42 -> 0.6, ~3 sim min of a tower's growth), so the plate stood in one or two frames at 5 min/s whatever the
  shield did. Each tower carries `wx::Tower::flareAge` (s since it passed `wx::kFlareOn` 0.42, from its cycle's peak and
  growS) and `updateStorms` turns it into a SPREAD on the same ramp over the formation time: anvilK goes from 1 / flare
  (no flare) to its value, and the spread rides in the GPU tower's c.z (anvilK + 8 x round((1 - spread) x 1000): a fully
  spread tower's c.z is its plain anvilK; `cv2TowerAnvilK` / `cv2TowerSpread`, cv2_storms.glsl), which scales `near`. A
  young Cb stands as a flat-topped column at the lid and spreads from there. Tower strength, life, lightning and
  `mature_in_min` are unchanged (3612 near-row pairs over 5 h: identical); a storm's `left_min` grows by its longer linger.
  Measured at the report's view (-2.50, 134.26, 10.4 km, 2036-12-25 03:14-08:14 UTC, 1-min paused steps, the 8 nearest
  shields, harness `weather state` -> `shields`): shield presence 10 -> 90% 3.8 min (old 15 min / 35 min smoothstep) ->
  21.7 min (median of 7, min 20.5); 90 -> 10% 14.8 (min 9.5) -> 43 min (min 32.5); the largest 5-min change of a
  shield's presence 95% -> 22% of its peak. Constants alone (45 / 70, smoothstep) gave 11.0 / 26.5 min and 42%, and left
  the plate's pop in the frame as it was (the band above the horizon 4% -> 38% cloud in 2 min, minutes 17-19); with the
  spread it is 4% -> 11% (the flat top) and the anvil spreads after it. At 2x (120 / 140): strength 10 -> 90% 41.7 -> 80.1 min,
  90 -> 10% 79.9 -> 141.5 min; presence 58.4 / 94.8 min (the longer rise overlaps the decay's start more). The march: 128
  registers both, +256 B binary, GPU 18.58 vs 19.08 ms (4 reps, within noise).
  **Tower growth time (2026-11-03, same issue, after a verifier's check):** with the anvil eased, the column itself
  still popped at the report's moment: the drawn height is smoothstep(0.02, 0.6, strength) (`cv2ColumnSigma` hK) and the
  storm cumulus round it smoothstep(0.05, 0.3, strength), and a tower's strength crosses those bands in ~3 / ~2 sim min
  of its 15-25-min growth, so at 5 min/s the mound, the column and its cliff-sided flat top stood in one or two frames.
  `wx::Tower::heightCap` = 1 - (1 - age / T)^3 over the "Tower growth time (min)" T (`clouds_v2.storm_tower_rise_min` 30,
  slot 452, 0 = off; `Settings::towerRiseS`; cyclone towers 1), packed into the GPU tower's c.w with driftK (driftK + 4 x
  round((1 - cap) x 1000), `cv2TowerDriftK` / `cv2TowerHeightCap`), caps hK (the column and `full`), the storm cumulus
  (`prox`, by cap^2: the cubic rises fast early), the mid layer's rise over the storm (`storm`, cap^2) and the weather
  stamp's cloud field round it (`s1`, cap^2). The head
  flares when the capped height passes 0.8 (`kFullAt` = 1 - 0.2^(1/3) of T): `flareAge` and the shield's `anvOn` start
  at the later of that and the strength's 0.42. Strength itself, the rain core, lightning (CPU and the GPU pass's
  `a.w`) and `mature_in_min` are unchanged (576 near-row pairs, rise 0 vs 30: identical). Measured at the report's view
  (03:14-04:00 UTC, 1-min paused steps, the column-top height over the cumulus in the frame, `top.py`): 10 -> 90% under
  1 min (T 0, the old behaviour) -> ~5 min (T 30; the visible part is the column's last climb over the 12-km storm
  cumulus) -> ~12+ min (T 60); the storm cumulus plateau first shows over the horizon at ~03:29 at ~7 km and climbs to the column's flat top at the
  lid by ~03:38-03:41, instead of nothing at 03:30 and a cliff-sided flat top at 03:32. March: 128 registers both, +256 B
  binary; GPU at the shot within noise (interleaved, 3 clean reps: new - old -1.9, -1.8, -0.1 ms on a shared GPU). Not changed: a tower that grows round a 10-km eye (06:45-06:50 in that sweep) can
  leave the eye inside cloud; at 06:49:58 with T 0 boxy, texel-stepped walls persist after a 340-frame settle (real,
  only with the eye at a tower's wall; not seen at 06:36-07:00 every 4 min with T 30 or 0).
  **Squall lines and MCSs (2026-10-23):** `StormKind::SquallLine` / `Mcs`, spawned by their own pass (`buildLineBin`: 3-h
  bins looking back `kLineMaxLifeS` 22 h, 6-degree cells, the flash climatology's monthly mean x `lineSystemFactor`
  (warm-season mid-latitude continents, the Sahel / monsoon band, tropical oceans) x an evening peak over land; mean
  `targetLineActive` 160 worldwide at moisture 1, ~15 at the reports' dates; "Storm complexes (x)" `storm_complexes`, slot
  325). A line of 3-16 REGENERATING tower slots (`TowerSpec::period`, `towerCycleAt`: a new cell every 45-75 min while the
  system's envelope [sysOn, sysOff] allows, each its own seed and a shift along the line) across the motion (9-17 m/s;
  an MCS's slower arc with cells straggling behind), the middle forming first and the ends dying first; squall lines
  180-520 km long. Their shield is a capsule along the line behind it (45% of the trail), radius 0.55 trail + the tower
  radius. The weather bake (`stormStamp`, from the shields' flowed copy `cv2EnvShields` and their env index) stamps the
  TRAILING STRATIFORM region (deck + rain, the weaker transition zone 12-55 km behind), a SHELF-cloud band 1-22 km ahead
  (stratiform type: low and smooth-based) and clear air 25-90 km ahead — all measured from the spine moved forward by
  the line's BOW (2026-10-23: `Shield::bowM`, packed into the GPU shield's c.w as 2 x whole km over the rain share; the
  middle towers stand up to ~90 km ahead of a straight spine, which put the band, the clear air and the trailing rain tens of
  km behind them, the shelf band inside the trailing rain), and no rain falls ahead of the line (`gRainFree`). Tried and
  dropped: a 3D shelf as tiered plume Gaussians along the gust front (invisible at the overcast's base height, ~150
  Gaussians). Not yet: a real shelf / arcus volume, bow echoes'
  rear-inflow notch, MCS remnant vortices. Harness `storms list [kind=] [n=]`, `storms go storm=<id> | kind=squall|mcs|super
  [day=1]` (day: the Sun > 20 deg there), `state` -> `weather.shields / line_systems / cpu_ms_split`.
  **Tropical cyclones (v1, 2026-10-23):** `wx::Cyclone`, not Storm records: one per (basin, season year, index), built and
  cached per season (`Impl::buildSeason`, six basins in `kBasins`: genesis box, seasonal peak / spread, mean count ~82 a
  year in all; "Tropical cyclones (x)" `storm_cyclones`, slot 326). A track is integrated HOURLY at genesis in the
  EARTH-FIXED frame (the cloud map drifts east everywhere; a cyclone tracks west in the trades) along a ROUTE (2026-10-23:
  `kRoutes*` in Weather.cpp, 3-5 climatological track families per basin — Cape Verde recurvers, Caribbean -> Mexico, Gulf
  coast, Florida / East Coast; East Pacific out to sea, up the Baja coast, into Sinaloa, toward Hawaii; typhoons into the
  Philippines / China or recurving to Japan; Bay of Bengal; Southern Hemisphere west then poleward), picked weighted by
  nearness to genesis, followed by pure pursuit 500 km ahead keeping 35-75% of its starting lateral offset, two slow heading
  waves (+-3-9 deg, 1.5-6.5 days), a 4-deg wobble, 10% stall a day or two; 4.5-7 m/s in the trades, 8-14 past its recurve
  latitude. Genesis is redrawn off land (its own Rng stream: other storms' draws unchanged); the Eastern Pacific's potential
  falls westward of 122 W; a remnant ends 48 h inland. Harness `storms season ... track=1` (6-hourly [lat, lon, vmax, land]); vmax relaxes toward a per-storm potential (22-82
  m/s, median ~35) x a cold-water fade at the basin's latitude, collapsing over land (`Env::land`, tau 9 h). Named from
  the basin's WMO list in order of reaching 17.5 m/s (2036's Atlantic / Eastern Pacific lists = 2024's rotation with the
  2025 retirements replaced; the others a slice run on sequentially). `cyclonesAt` gives each one's state now (map frame
  from the inverse of `mapToEarth`; category, pressure from Atkinson-Holliday, radius of maximum wind contracting with
  intensity, eye 0.45 rmw past 33 m/s, CDO 120-300 km, bands to 350-800 km). Drawn as: an eyewall of 18 regenerating
  towers at the rmw (12 drew its rain as separate cores on the radar) turning cyclonically (a quarter of `Cyclone::turn`, the
  hourly integral of vmax / rmw: rate x age jumped as the storm strengthened) + 6 inner-rainband towers on the bake's band crests (`cycloneTowersAt`, heads blown outward; they do not flare, the CDO is the anvil: `updateStorms` gives Tropical towers anvilK 1/flare, driftK 0.25, so their reach drops from ~60 to ~28 km — cloud bucket at Medium 12 km in the eye 15.2 -> 12.5 ms, sea level 8.0 -> 6.5);
  the CDO as a shield with an eye (`CV2Shield.d.w` < 0 = -eye radius, `cv2ShieldAt`); and in the weather bake
  (`CV2Cyclone` x 8 in `stormBuf`, at the flowed centre: `stormFlowedDir`, the one CPU replica of `cv2FlowDisp`) main
  log-spiral arms trailing the rotation — `CycloneState::arms` of them (d.w: 2 for a tropical storm, up to ~5 at Category 5,
  crossfaded only over the last 15% before the next whole count: half-and-half mixes interfered into rings), pitch 30 -> 22
  deg with intensity (CPU `tanPitch`), turning by `bandRot` = 0.04 x turn — attached to the CDO, typed convective, their
  coverage near solid at the core (at 0.45 a weak storm's arms drew as scattered cumulus) fading to 0.5 outward (wisps),
  THIN (2026-10-23, the user: "pudgy": an arm covers the share of its period where cos > 2 thr - 1; thr 0.8 -> 0.93
  outward, ~23% -> 15%, was 0.32-0.72, ~44%), pinching along the arm, feathered by fine parallel streaks at the edge, edges
  ragged by the map read at a permuted direction; + three finer arms (22 deg), breaking up outward (13-20 deg detached arms read as nested rings); the bake also
  reads the MAP through a static differential rotation about each cyclone (`cycloneSwirl`: cyclonic, 0.6-1.5 rad near the
  CDO, none past the outer bands), so the map's own cloud near a storm trails round it in spirals, the CDO deck,
  eyewall rain and the eye cleared outright (the moat at x4). Its wind drives the surface wind (`surfaceWindAt`: rising to
  the rmw, (rmw / r)^0.6 beyond, 20 deg inflow, x0.7 at the eye's height, calm eye) and so the rain, snow and ambience
  (the open-sea surf `sea_open` louder, fizzier and quicker with `surface_wind`), and the SEA (2026-10-23: the two nearest
  in CloudParams `seaCycA/B`, 11040 -> 11104, `weatherFillSeaParams`; `oceanSeaState` takes max(its state, (U / 9 m/s)^1.3,
  at most 5) on the same profile: whitecapped seas, surf on a landfall coast). OUTFLOW cirrus: a second, weak (0.25), aged
  (0.85) shield per cyclone to max(1.6 CDO, 0.6 outer) — the orphan-anvil look; at 0.4 it drew grey blotches between the arms.
  ARM CIRRUS (2026-10-23, the user: "wispier"): per arm a chain of 4 thin, aged (0.9) capsule shields along its crest (the
  bake's spiral), 0.75 CDO .. 0.95 outer, a quarter of the arms' spacing wide, strength 0.32 falling 55% outward: frayed ice
  veils feathering the bands. Up to ~22 shields a cyclone, so a bin entry's count is 5 bits now (`y >> 5`, `& 31`, every
  storm index; kShieldMaxPerBin 31; the march stays at 128 registers).
  **SHAPE (2026-11-03, issue 20261030_205220, `wx::CycloneShape`, Settings > Weather > Storm objects, slots 455-461,
  `clouds_v2.storm_cyc_*`):** "Hurricane main arms" (`arms`, default 2, 0 = by intensity as before; most satellite images
  show two major bands), "arm tightness" (x the pitch's cotangent), "arm speed" (bandRot = 0.04 x speed x turn) — all
  three applied to `stormCyclonesNow` by `wx::applyCycloneShape` before `cycloneTowersAt`, so the band towers, the bake
  and the deck agree; the bake reads the pitch from `CV2Cyclone.d.z` = over land + 2 x round(1000 tan pitch). "minor
  swirls" (`pc.storm2.z`): 2n + 1 finer bands wound at 0.8 the pitch, broken by their own gaps (were 3 at a fixed 22 deg).
  An arm is as wide in angle whatever the count (b^p, p = (4 / n)^2: the thresholds were tuned at ~4 arms; 2 drew fat
  ribbons). ARM GAPS: an arm narrows to nothing along its own length (a wave in ln r with the arm's own phase, its crest
  index mod n — periodic across atan's cut — raising the threshold). They were a multiplier `brk` of (ln r, theta) waves
  whose zero lines cross every arm at an angle: arms cut off square — the report's "chopped in rectangular ways"
  (shot 2 with `brk` = 1: every blunt arm end gone); `brk` and `pinch` also took 1.3 / 2.1 / 1.7 x theta, not whole
  multiples, so atan's cut drew a straight radial seam across the arms: every angle term is now a whole multiple of
  theta (arm indices mod n). An arm's core is smoothstep(thr, thr + 0.06, b): with the old min(thr + 0.06, 0.995) a
  threshold lifted past ~0.94 (a gap) put edge0 > edge1, which NVIDIA evaluates inverted — the whole space between the
  arms filled on one side of the gap's edge, a straight-edged wedge. "deck spiral" (0.8): the CDO's low deck (`inner`, the deck floor) reaches out along the
  arms and recedes between them (r x (1 - 0.8 k (bA - 0.5)) x (1 - (0.4 k)^2)^-0.75, `pc.storm2.w`: the last factor
  keeps the wound deck's mean area the round one's — 17% more overcast, raining deck cost ~1 ms), and the CDO shield's
  outer part is notched between them (`cv2CycSpiral` mode 0). THE CIRRUS DECK replaces the outflow disc + 4 x n
  arm-cirrus capsules (chords of ~40 deg of the spiral, each thinning to 0.3 at its far end: a polygon of bars stepping
  in brightness): one disc per cyclone (max(r1 / 1.1, outflow reach), its eye hole r0 / 1.6) on which `cv2CycSpiral`
  mode 1 draws the arms' cirrus on the low arms' own spirals, the minor swirls, and between them the outflow cirrus
  ("outflow cirrus" x strength and reach) in 3n + 2 thin streaks curving ANTICYCLONICALLY ("outflow streaks", contrast).
  REVIEW (2026-11-03, the wave verifier): (1) the deck's outflow streaks were gathered to 2x on their crests and the
  disc, ~700 km, was frayed like an anvil's edge (6-7.5 x the cluster noise): its thin sheet drew the noise's contours
  as flat grey patches with hard edges between the arms. The streaks now only thin the sheet (o x mix(1, st, m)), and
  the deck takes the shield's radial fade inside `cv2CycSpiral` while `cv2ShieldAt` hands the caller u / 10 for it
  (no fray). (2) The CDO read as a featureless disc inside ~0.8 of its radius (2-arm share 0.002-0.03): the moat
  between an arm and the CDO is NARROW and runs in to about half its radius, as feeder bands wrap in on GOES / Himawari
  visible imagery (the core stays uniform): the low deck's radius x (1 + 0.7 k) over bA < 0.35 and x (1 - 0.25 k) over
  bA > 0.5 (mean area 1.00 at k 0.8), the CDO shield thinned to 1 - min(1.15 k, 0.95) from u 0.3-0.6 over arm < 0.45.
  Shot 2, 2-arm share at 90 / 110 / 140 px: 0.004 / 0.011 / 0.030 -> 0.034 / 0.040 / 0.068, phase +111 -> +176 ->
  -167 -> -94 (170 px) deg; 50-70 px (the core) unchanged at 0.003.
  The spiral rides in the shield (`Shield::spiral`): c.w < 0 a packed code -(1 + mode + 2k + 32o + 512m + 8192a +
  131072 ro), d.xyz
  hemi x arms / 1 / tan pitch / phase; `cv2ShieldAt` applies it once a sample, to the strongest shield only (x <= 1: the
  strongest-first early out stays exact). `clouds_v2.storm_cyc_spiral false` (no UI) = the old deck, the A/B. Shot 2
  azimuthal luminance round the eye (harness_runs/c03 azprof.py): the dominant harmonic is h2 at every radius 120-280 px,
  its phase drifting -169 -> +36 deg outward (a log spiral); before, inside the CDO h2 was 0.010-0.019 of the mean
  (now 0.041-0.134) and outside the peak harmonic was h5 with no steady phase. Perf (RTX 3070 Ti, 1600x900, the
  report's settings, ABAB x 4, p50 GPU): shot 1 26.15 -> 25.68 ms, shot 2 19.14 -> 18.94; march 128 registers, binary
  382592 -> 389376 B. The cirrus deck's outflow ends at ~1.25 of its reach (packed `ro`): over the whole disc it kept
  cv2AnvilSigmaP's texture reads alive between the arms (+3 ms at shot 1).
  **THE CDO'S TOP (2026-11-05, the user: "a nice smooth but textured swirl, right now it is a bumpy mess"; ref
  qa/waves/20261105/refs/hurricane_iss.png).** The CDO shield's plate went through cv2FieldLow's storm lobes and billow
  erosion like any anvil (the grey dimpled sheet with dark erosion pits seen from 30-160 km) and at the anvil's ice it
  was translucent: a grey sheet over the darker deck below, beside the white eyewall heads. Now, when the strongest
  shield under a sample is a cyclone's CDO (d.w < 0, not the cirrus deck), `cv2ShieldAtR` reads the SWIRL TEXTURE into
  `gCv2CycT` (-1e9 = none): the shape volume's Perlin in log-polar coordinates about the eye (x = turns x 8 + the arms'
  phase, so it turns with them; y = 6 ln r + 8 turns: a ~12 deg inward spiral; whole periods, seam-free), ridged
  (1 - 2 smoothstep(0, 1.2, |sd|), faded from LOD 3-6, the LOD +1 because a grazing view looks across the ridges: at +0
  they drew moire spokes behind the eye) + 0.15 x the billow channel (at 0.4 its cells drew scales), calmed within ~4 eye
  radii (the eye's rim is a plate edge too: a fingerprint of rings); streaks ~r / 25 apart, ~5x as long as wide. Its
  footprint is `gCv2FpM`, set by cv2Field with the reset. cv2Field adds RIDGES to the storm margin where the plate is the
  margin (deck > 0.6): sg.m += "Hurricane top streaks" (`clouds_v2.storm_cyc_top_streaks_m` 90 m, ciHu.w) x (1 + 10
  sg.cap: toward the plate's outline, which frays into streaks along the swirl instead of a lid's step) x max(T + 0.3, 0).
  Never negative: the plate's margin is the distance to its lid OR its underside, so an offset thickens or thins it, and
  a thin plate thinned into holes; at 160 m the ridges seen obliquely from 30 km were dark scales. It multiplies the
  plate's ice by 1 + ("Hurricane top density" `storm_cyc_top_density` 3, ciHu2.x - 1) x deck. cv2FieldLow removes
  "Hurricane top smoothing" (`storm_cyc_top_smooth` 0.85, ciHu.z) x wS of the storm surface's lobes (lobeKL, lobeB), 0.94
  of that of its erosion and all its billow, over the plate and the eyewall heads alike. Register cliff: the texture fetch
  in cv2FieldLow, or the relief added to its c.m0 there, put the march at 248-250 registers (128 without); the fetch in
  the shield walk and the relief on sg.m in cv2Field keep it at 128. Not in SKY_LITE (CV2_CYC_TEX). Cost (interleaved A/B in one app, the old march SPV
  against the new, 3+3 reps, GPU shared with three other agents so absolute numbers are contended; cloud_march bucket,
  ms, A-vs-A floor in brackets; qa/waves/20261105/evidence/cirrus-hurr/perf_ab.txt): Yun-yeung from 160 km 82.3 [79.7]
  -> 28.5, from 30 km 50.0 [47.9] -> 28.5, from the ISS (400 km oblique) 27.6 [28.1] -> 25.4: the plate's lobes and
  erosion were most of a hurricane's march cost (their lobe bound kept every sample near the sheet fetching). 128
  registers.
  THE PLATEAU (2026-11-05, storm-above): the "whiter, flat-topped plateau" beside Yun-yeung's eye from 30 km was not the
  eyewall heads: it stayed with the towers knocked out, the heads' hang, the near-head lid smoothing, the CDO
  smoothing, the veil, the mid and high layers, the low field above 7 km and the ambient / bounce terms; with the
  plate knocked out a flower-shaped low cloud mass stood under it. The plate was TRANSLUCENT ("Hurricane top density"
  3; the setting was also clamped at 6), so the white low mass showed through it beside the dark sea. Default 12, range
  1-20 (`clouds_v2.storm_cyc_top_density`): from 160 km the white patches round the eye are gone, from 30 km the
  plateau is a faint step. The domes' black blots were the march's shell top cutting them flat (clouds.md, "Storms from
  above"). Still open: the spiral bands outside the CDO are the low cloud's popcorn, not cirrus-topped (the arm cirrus
  deck is thin); the eye is a clean cylinder, not a stadium sloping outward.
  **STORM SURGE** (2026-10-23, terrain_detail.glsl `tdSurgeAt`, terrain.glsl `waterAdjustHeightSurge`, CPU
  `weatherFillSeaParams` + mirror `surgeAtDir`; UBO `surgeA/B[2]` + `surgeInfo` appended, 11104 -> 11184): per seaCyc
  cyclone a peak rise at the coast from the wind of the last ~6 h (max(vmax, 0.85 vmax 3 h ago, 0.7 vmax 6 h ago): 1.2 m
  at 33 m/s, 3.3 at 50, 6.4 at 70, at most 9; x (rmw / 40 km)^0.3; x "Storm surge" `clouds_v2.storm_surge` 0-2, default 1,
  no UI row yet) x a field in the storm's frame in rmw units: across the track a Gaussian about 1 rmw on the right of the
  motion (left in the south: `surgeB` = hemi x motion x up), sigma 1.8 outward / 0.9 inward; along it sigma 4 ahead /
  2.5 behind (Nadine 2036 at 29.4 N 90.3 W: 0.3 m 12 h before the peak, 1.6 at 8 h, 4.3 at landfall, 0.9 8 h after) x
  the shelf (the DEM's mip-5 mean: flat = 1, high land 0.45). Only the SEA rises (`level == 0`: the sea and sea-level
  water): it takes the rise, tapered to 0 from 0.45 to 0.95 of the water map's reach offshore (the open ocean is drawn as
  before), and its shore moves inland by rise / the plain's slope (that mean / 40 km, 0.25-20 m/km), at most 0.7 of the
  water map's reach (~14 km), only through the DEM's lowest land class (30-60 m: the 8-bit DEM cannot tell 2 m from
  30 m). `tdDemAt` (so the march, both depth passes, the observer's ground) and `shoreSignedDist` (surf, beaches: none on
  a flood line) take the moved shore; the coves follow it. The sea at the surge's level is a lake hit for the march; a
  ray that steps over it re-intersects the sphere at that level (`waterLevelM`). Over flooded land the waves are
  depth-limited and the water muddy (`gSeaFlood`, fading out ~8 km offshore). Gated by `surgeInfo.x` (uniform) then
  each storm's reach (10 rmw); not in SKY_LITE / SKY_ENV (Planetarium's SPIR-V +200 B: the UBO's declaration). Cost
  (Medium, 1600x900, RTX 3070 Ti): 8 km over the flood scene depth +0.2 ms, total within noise; at the flooded coast the
  sky pass 0.7 ms CHEAPER (water instead of land shading); far from a cyclone 0. Harness `state` -> `weather.surge_m` (at
  the eye: the field x the shelf from a 9-point DEM mean) and `weather.surge` {field_m, shelf, peak_m, flood_reach_m,
  shore_dist_m, dem_m, flooded}; scripts in `harness_runs/surge/` (untracked). Not yet: no bathymetry (the shelf is the
  land's flatness), the flood drains with the storm field (no water left behind), no drawdown on the left side, no
  Bangladesh-scale delta check, sea-level lakes behind levees (Pontchartrain) flood as sea.
  Harness `storms cyclones`, `storms season basin=<0-5> year=`, `storms go cyclone=<name|id|strongest>`. Not yet:
  extratropical transition's look, the eyewall's lightning share.
  **Tornadoes and waterspouts (2026-10-23, `SatelliteSim::tornadoOf` / `weatherFillTornadoes`, SatelliteSimWeather.cpp):**
  fixed by their storm's id: ~35% of supercells put one down for 5-25 min of their mature phase (between `anvOn` + 20 min
  and `anvOff` - 10 min), ~12% of strong single / multicell storms over the sea a waterspout (4-14 min); on the rear
  flank's rain-free base, ~a tower radius behind and across the shear from the tower's axis (inside 0.6 of it the rain
  core hid it). NOT in the cloud field: drawn as plume Gaussians in the dust list after the launches' (`col.w` = 2 +
  albedo, cloud_march.comp composites them with the Earth-fixed billow field below ~10 km): a funnel of 10 dense, narrow
  Gaussians (peak tau 40 at sigma rr / 2.6, so the edge, tau ~1, sits at the radius rr: at tau 3.5 and sigma rr it was a
  soft cone) widening 2.2-3x to the base, lowering over its first ~90 s and roping out (thinner, bent) over its last
  third, a faint wall cloud and a debris cloud (spray at sea). Lit by the sky (~0.06 of the plumes' clear-sky unit) and a
  little Sun: it stands under the storm's base. **It hangs from the DRAWN cloud base and is lit through the storm's column**
  (2026-10-23, issue 20261023_223907 "tornado tops do not connect to clouds ... do not abide by lighting"): it hung from the
  Cb TYPE's base (`cv2Types[4].alt.x` above the CPU ground), hundreds of metres under the cloud drawn there, and was lit as
  under a clear sky (a white funnel under a dark grey base). `weatherFillTornadoes` asks cloud_v2_lightning.comp for up to
  four columns (the nearest funnels: Earth-fixed directions in `cv2SunProf[8..19]`, written by the HOST); threads 2-5 find
  the first height from the ground where the field (rain and thin ice left out) passes 1e-3 /m (24 x 250 m, 5 bisections)
  and the optical depth above it (12 steps to the shell top), packed into `cv2RainMisc` (uvec4: base m ASL | tau x 64 << 16;
  0xFFFF none); the host reads them the next frame, eases them per storm over 0.4 s (`tornadoProbes`), runs the funnel 250 m
  INTO that base (the cloud in front hides the join) with the wall cloud 60 m under it, and lights it as the march lights
  that base: x lightK = (Td + 0.35 (0.3 + 0.7 Td)) / 1.35, Td = 1 / (1 + 0.11 tau), its sky hue kept by Td (grey under a
  storm). No probe (clouds v2 off, from orbit): the type's base. ~40 field evaluations per probed funnel on one thread of the
  lightning pass; GPU within noise. Harness `state` -> `weather.tornado_list` (base_agl_m, base_probed, tau_above, light_k).
  **It is opaque** (2026-10-24, the user: "Tornados are transparent. We can see a lot through them"): the plume composite gives
  back 0.85 x albedo x (1 - exp(-tau)) of the BACKGROUND (the sky light a plume scatters, for plumes without their own), and
  a tornado carries its own (the iso term x lightK): 38% of whatever stood behind the funnel showed through its core, under a
  storm base far more than its own light. Tornado Gaussians (seedK >= 256) now add no albedo to that give-back (`Ls`,
  cloud_march.comp), keep 0.3 of their optical depth in the billows' troughs (0.05 punched holes), the debris cloud is 6
  across (spray 3; was 2 / 1.2) and the wall cloud 8 across, sigma 700 m (300 at sea), 200 m thick (was 1 / 1500 m / 160 m:
  translucent and ~9 km wide). Background blocked in the funnel's core (1 - on / off, linear, 1600x900): 0.40-0.41 -> 0.67-0.77
  median, 0.84 p90 (the rest is its own dark-grey light); composite 128 registers, cloud_march within noise. Open: the
  plume's front / behind blend keys on the cloud's MEAN distance, so mist near the eye (the report's view, 125 m under the
  base) still veils a funnel the mean puts behind it; the opacity median (`tCloudOcclude`) instead drew the wall cloud as a
  dark mushroom in front of the base (tried, dropped). The wall cloud's edges where the base's distance crosses it are hard.
  Harness `storms tornado draw=0|1` hides / shows the funnels (an A/B of what shows through them).
  **Its roar** (layer `tornado_roar`, synth wind: low brown rumble 130-360 Hz, broad, fast churning gusts and flutter, debris
  hiss close; driver `tornado` = sum over the funnels within ~200 km of sqrt(width / 150 m) x lifecycle / (1 + (d / 2 km)^2),
  d to the nearest point of the funnel; heard below ~3-6 km AGL). Offline (`audio record ... solo=tornado_roar`): -36 dBFS
  RMS 1 km away (the whole ambience -35), -39 at 3 km, -48 at 12 km. It SPINS (2026-10-23): seedK >= 256 marks a tornado Gaussian whose
  seed carries a spin phase (3840ths of a turn: the funnel ~35 m/s at its radius, debris 0.7 x, wall cloud a turn in ~5 min,
  cyclonic), and cloud_march.comp reads the low billow field in LOG-POLAR coordinates about its centre turned by it
  (`tornadoCyl`: radius cells would draw the wall cloud as rings). The plume composite blends in front of / behind the cloud
  over +-(15% + 150 m) of distance (a hard switch cut a cloud-shaped hole round the funnel top). Placed at one of TWO
  places on the rear flank (2026-11-08, issue 20261108_173628 "Where's the tornado here?"; `tornadoOf` -> `mapDir` /
  `mapDirIn`; the direction -0.65 shear + 0.7 across is 0.955 long): OUTER, 1.1 x the mature tower's own radius Rc
  (0.7-1.3 x "Cb tower radius" by size; ~1.05 Rc), past the rain core (to 0.9 Rc) -- the rain-free base when the map's
  low deck reaches past the tower there; INNER, 0.8 x (~0.76 Rc), under the tower's own base just inside its edge, where
  the rain core has faded to ~5%. weatherFillTornadoes probes both columns (the four probe columns hold two tornadoes'
  pairs; with more, a window of four rotates a frame at a time) and draws the funnel at the outer place while a storm base
  stands over it, else at the inner one (`TornadoProbe::choice`, kept while its base holds; a move fades the funnel in
  again). The outer place alone stood PAST the drawn tower's base (its body fades out by ~0.85-0.9 Rc, its base rising
  there): over clear air the probe gate below hid the funnel while the Storms tab listed it on the ground. Probed at the
  report's storm (Argentina, Rc 21 km with the reporter's 20-km tower radius): 1.1 no base; 0.9 a base 2.65 km up; 0.75 /
  0.6 / 0.45 / 0.3 a base 1.0-1.15 km up, tau above 900-1000; captured, the funnel shows at 0.8 and is veiled by the rain
  core at 0.7 and in. At another (2036-11-25 off Brisbane) the outer place had a base 1.1 km up and a clearly drawn
  funnel, while at 0.8 it was lost in the rain (light_k 0.09 under tau ~1000, against rain): neither place alone works for
  every storm. The Watch views, `storms tornado` and the featured search frame the OUTER place (an inner funnel stands
  ~0.3 Rc nearer the tower: in frame from their distances). `state` -> `weather.tornado_list[]` also carries `placement`
  (outer / inner), `tower_km` (the drawn place from the dominant tower's axis), `rc_km`, `found` (the drawn place's last
  probe found a base), `found_outer`, `found_inner`. Cost (contended RTX 3070 Ti, 1600x900, the report's view, `storms
  tornado draw=1|0` interleaved x3): cloud_march within noise (funnel in frame +0.0-0.15 ms, out of frame +-0.2 ms; the
  A-vs-A spread 0.1-0.8 ms); the second probe column per tornado costs ~40 field evaluations on an idle thread of the
  lightning pass. The bake clears a
  rear-flank rain-free base (centred 1.1 Rc, cleared within 0.35 Rc: it covers both places) and suppresses the map's rain inside every storm envelope (x 0.05: "Rain" 3 saturated what was left).
  Only within ~250 km, by day. Harness `storms tornado [spout=1]
  [dist_km=] [alt=] [el=] [az_from=]`: the next daylit one among the storms alive over the next 12 h, time set into it,
  the observer on its inflow side; `state` -> `weather.tornadoes`. Known: a neighbouring storm's forward flank can still
  rain over the view (harness_runs/wx4: 2036-11-22 14:00); the cloud field inside a hurricane does not swirl (its noise is
  map-fixed; the flow is one global curl field).
  **Still transparent close up (2026-10-26, issue 20261026_191508)** — two causes: (1) cloud_march.comp's "a Gaussian the
  EYE is inside thins to 10%" (meant for a rocket's exhaust beside a followed stage) took the funnel's wide base and debris
  Gaussians within ~1 km: tornado Gaussians (ib.w >= 1024) are exempt; (2) the unified composite dimmed a plume's light by
  the cloud opacity at its depth SEGMENT's middle, and in rain a segment runs from a few hundred m to tens of km: a
  plume's light is now corrected to the opacity in front of its own closest approach, and the cloud light of its
  segment is dimmed only behind it (`sU`, the plumes' tau-weighted share of the segment's cloud in front of them; it
  was half their optical depth for all of it). Composite at 128 registers. **Terrain gate:** a supercell's tornado
  needs ground under 1000 m with under 400 m of relief within ~20 km at its touchdown (`tornadoOf`, CPU DEM; it touched
  down in the Nepal mid-hills); waterspouts unchanged.
  **A tornado needs a storm base over it (2026-10-27, issue 20261027_212628 "Tornado formation under absolutely zero
  cloud cover", a supercell 16 km off over the Argentine plains, the funnel standing in clear sky).** The probe
  (cloud_v2_lightning.comp threads 2-5) had found NO cloud in the column (0xFFFF, `base_probed` false) and the funnel hung
  from the Cb type's base anyway. Now the funnel, wall cloud, debris and roar are x `TornadoNow::present` (eased per
  storm over ~0.25 s in `TornadoProbe`), 1 only while the LAST probe found a base within 3.5 km of the ground with a
  tau >= 3 of cloud above it (`kTornadoMaxBaseAglM`, `kTornadoMinTauAbove`, weatherFillTornadoes); never probed (clouds
  v2 / the lightning pass off): none. Over that storm's spot the column held rain (from the anvil, 16 km from the tower
  with the user's 15-km "Cb tower radius") and the anvil above 6 km, no low cloud; a coverage floor at the stamp's
  rear-flank rain-free base (cloud_v2_weather.comp) was tried and changed nothing there (reverted). Three other
  tornadoes found by `storms tornado` (2037-01-16 .. 19) have bases 1.0-2.4 km up (tau above 530-1020) and draw as
  before. Harness `state` -> `weather.tornado_list[].present`.
- **Biome map + wildfires (2026-10-23, reworked 2026-10-24 from issue 20261023_225322 and again the same day from the
  user's QA; `Biome.h/.cpp`, `Wildfire.h/.cpp`, `SatelliteSimWildfire.cpp`, `shaders/wildfire_ground.comp`,
  `shaders/include/smoke.glsl`; plan `.plans/WEATHER_HAZARDS_PLAN.md`).** `wx::BiomeMap`
  (2048 x 1024 equirect, built at load in ~17 ms on 12 threads from the day map's 3x3 colour, latitude, the CPU DEM and
  the ocean mask): water, ice, tundra, boreal / temperate / tropical forest, grassland, savanna, shrubland, desert, + FUEL,
  DUST and URBAN bytes; `seasonalDryness(lat, doy)` (extratropical summer, tropical dry season: north peak ~day 15, south
  ~220). **URBAN** = the night map's lights (`earthNightCpu`, ~18-km box means of sRGB luminance, the brightest of nine
  reads over the texel and its edges; smoothstep 20 -> 34, the land's blue base ~14): `burnableAt` = fuel x (1 - urban),
  and no fire starts or runs where it is 0 (the user: "they seem to generate over cities"). Harness `biome [<lat> <lon>]
  [export=<png>]`. Kansas reads shrubland (the day map's tan prairie). **Fires** are a pure function of sim time: hashed
  per (1-degree cell, 1-day bin), rate 0.045 x burnable x dryness^2 x cos(lat) x "Wildfires (x)", ignited mostly in the
  local afternoon, log-normal life (median 2.5 d x size, 0.4-30 d), a burnt ellipse growing along the spread direction
  (`fireStateAt`: activity = growth x decay x a diurnal cycle peaking ~15:30 local; head / back / half width; plume top
  0.8-3 km, x2.6 for the rare pyroCb). Measured on 2036-08-12: 418 burning worldwide before the urban mask, 340 after
  (the lit farmland of India, China and Europe lost most); `fires world=1` -> `world_front_on_lights`: of ~3600 burning
  front samples, 1 on a texel of luminance >= 30 and 0 >= 60 (were 74 / 34 with the first, looser mask). The sim caches
  the fires born round the eye (`firesBorn`, per day bin; rescan on a move > 15% of the reach) and each frame
  (`weatherFillWildfires(out, n, pixFull)`, after the haboobs in the dust fill) draws, nearest first, within 600 km (to
  3000 km from orbit; faded over the reach's last 20%).
  - **The wind** (`wx::windAt(dir, h, t)`, m/s east / north): a pure function of place, height and time, coherent in
    space (neighbouring fires' smoke agrees): a zonal climatology (trades, westerlies growing with height, polar
    easterlies) + the CURL of three sets of travelling waves on the sphere (synoptic ~3000 km / days, mesoscale ~400 km
    / hours, small ~30 km / an hour: the wiggle), slower and backed ~25 deg near the ground (friction), turning with
    height by a slowly varying thermal-wind angle (up to ~+-35 deg by 3 km). A fire spreads along the field's 10-m wind
    a third into its life (`Fire::windAz`, the burnt ellipse's fixed axis: it does not turn); its SMOKE follows the
    field. It replaced a per-fire hashed direction (trades or westerlies +-75 deg) with a slow wander that ROTATED the
    burnt ellipse — the user's "wind seems random, in only one direction".
  - **The front is an ARC** (`fireFrontHeat(f, s, a, t, bio)`, 2026-10-24; the user: "fire generates as rings, at best
    they should be arcs with scattered flames here and there"): the burnt ellipse with FINGERS (`fireFrontAt` =
    the ellipse's point x `fireFrontRadiusK`, a radial perturbation ~10-24%) burns all round only while young; its
    ACTIVE ARC narrows over 1-14 h to a half angle of the ellipse's parameter of 50-80 deg (the head and the upper flanks of
    an ellipse ~3x as long as wide), turned off the axis by a slowly varying skew (one flank runs harder), x 0.25-1 toward
    the head; broken into RUNS of flame and gaps by a coarse noise (18-32 cells round, re-drawn over 1.5 h) thresholded
    so ~40% of the head and more of the flanks are out, NOTHING burning in a gap (the old pattern floored every gap and
    the back at 10-12%: summed round the perimeter it drew a closed ring); a finer variation (120 cells, 0.6 h); a few
    isolated flare-ups off the arc (fewer as it ages); x the biome map's burnable at the front's point (the front stops
    at a lake, a town, bare ground). Behind the arc the ground is BURNT (below).
  - **The front's points** (kFireBuf, `kessler_fireballs.comp` appends them; drawn like the city sprites): on a NESTED
    hierarchy round the perimeter (finest 2^14; 64 base segments subdivided by their nearest range), each point's level
    from its OWN range so they stay 3.5 px apart on screen near, 2 px from afar (50-300 km: a front then reads as a line
    of light, not beads), x `fireLodScale` (widened past 3500 a frame, relaxed slowly); the next level's points fade in
    carrying half their parents' light (energy kept, no pops). Per point: the flame (on the active arc, jittered across
    the front by the flaming zone's depth), a burning-out zone just behind a burning stretch (45%, a few hundred metres
    deep by the spread rate x 0.25-2.75 h, red, fading with the time since the front passed), the burn scar's sparse
    SMOULDERING spots (6%, anywhere toward the centre, dimmer inward; not a second ring), spot fires ahead of the head
    (8%, over burnable ground). **Brightness**: PHYSICAL (luminous intensity per metre of front = flame luminance 2e4
    cd/m^2 x (flame height x cos + flaming-zone depth x sin of the view's elevation) x heat x activity, x the stretch of
    front it stands for, / r^2, extinction -> a magnitude -> effectFlare, / (1 + 30 x the Sun at the fire) by day), then
    `fireShoulder` = 0.9 (1 - exp(-(x / 1e4)^0.45)): a big head 5 km off at night ~0.8, from LEO ~0.4 (about mag 2.3),
    the faint back and embers from orbit ~0.06. A flare of 0.5 is the point style's white peak (pointRefMag 1.5), so no
    point swells past a star's core. Until 2026-10-24 it approached 8 (16x that white, each point a swollen disc; a big
    head from LEO ~5.5): the user's "way too bright, overwhelming even from space". Flicker ~1 / sqrt of the front a
    point stands for (0.07-0.55). **A light on the ground is dimmed by T, not T^2** (`sat_point.frag`: city lights and
    fire points, `fragManualDepth`): the dust haze over a dry-season savanna (T ~0.3 along a grazing path 100 km long)
    squared hid every front past ~50 km.
  - **On the drawn ground** (`wildfire_ground.comp`, `recordWildfireGround`, right after the dust fill and before the
    cloud composite and the points): the CPU places points and flames over the DEM it has (the full DEM at the ignition +
    the 18-km copy's change across the fire); the ground drawn is the DEM + the procedural detail, tens to hundreds of
    metres off in rough country, so close up most points sat UNDER the drawn surface and the depth test hid them (the
    user: "they disappear when you are close": from a 600-m hill 5 km off none showed). Each point (a NEGATIVE range =
    the CPU's mark; its height over the ground rides in its direction's length, 1 + h / 1000 m: the flame half its
    height up, embers 1-1.5 m) and each flame Gaussian (its height in `ib.w`: emissive Gaussians read no ib.w) is moved
    onto `terrainHeightDetailed` at the march's own LOD (`tdGeomLodM` of its range) and its direction / range / view cone
    rewritten. One 64-thread dispatch over ~2000 points: GPU within noise. Without the snap pass (it failed, or terrain
    detail is off) kessler_fireballs.comp takes |range| and the normalised direction.
  - **Burn scars** (`sat_sky.frag` `fireScarAt`, UBO `fireInfo / fireC / fireU / fireV / fireAux / fireK`, 11184 ->
    12224 B, `weatherFillFireParams` from last frame's `firesNow`): the nearest 8 fires' burnt ellipses with the same
    fingers (64 radial factors a fire, packed in bytes) in the q frame: the day map goes to charcoal (0.028) with
    grey-white ash patches (~110 m) and unburnt islands (~300 m), averaged to their means past 60-400 m a pixel, never on
    lit land (the night map's lights there); faded over the last 15% of the fire's life (the scar ends with the fire:
    not yet a scar that lasts months). Not in SKY_LITE / SKY_ENV. From orbit by day a fire is a dark scar with the smoke
    leaving its active edge; at night the points carry it.
  - **Smoke from the whole active front** (2026-10-24; the user: "smoke trails only generate from singular points, and
    not the full fire circle / arc"): the front's heat (48 samples x their length) is cut into S sections of equal heat
    starting at the arc's coldest point (5 within 150 km, 3 within 500, 2 within 1200, else 1); each section is a SOURCE
    at its heat-weighted centre, 120 m + 0.4 x the flaming depth downwind, as wide as the section (1.2 x its rms extent /
    sqrt S), carrying its share of the fire's smoke and rising to its own layer (`fireInjectionHeightM` x (0.7 + 0.3
    sqrt(share x S)), 0.55-1.1: a weaker section injects lower); 20% of the strongest source's goes to the low layer (0.4
    x, near fires). Each source is a chain of PARCELS rising (`fireSmokeRiseM`, the updraft `fireUpdraftMps` 5-19 m/s)
    toward their layer, drawn from 0.12 of the rise time on (the hot gas right over the flames is clear: started at the
    ground, the first Gaussian's tails hid the flames from the side); a stretch's mass = the source rate (`tau0` 2.5-8 x
    the head front's width) x its share x the fire's activity WHEN it was emitted. Plus one flat, MASS-CONSERVING **pall**
    per fire (the smoke older than the streaks, ~4-day removal, 20-350 km: ~0.01-0.05 from above each). Up to 384
    Gaussians a frame (~60 a near fire). A contiguous
    range of the dust list (with the haboobs), its bounds in the list header's word 1 (bits 8-17 first, 18-27 count; bits
    0-7 the engine lights' count: every reader masks `& 255`).
  - **The smoke follows TRAJECTORIES** (2026-10-26, issue 20261026_204239: at 5 min/s "smoke kind of just wavers around
    randomly and sometimes just changes direction entirely ... changing wind direction at the fire's source should have
    no effect on smoke farther down the trail"). The streak was re-walked from the fire every frame with each stretch's
    wind taken at the time that stretch's AGE ago: every knot sat on the walk through the source's NEWEST wind, so a
    turn of the wind at the fire swung the whole trail as one body, and the pall was put at the trail's end + its wind
    now x up to 18 h (hundreds of km a swing). Now each parcel has a fixed EMISSION time on a nested grid (spacing d x
    2^l for the ages in band l, m knots a band: d = 60 s and m = 2 for a near fire's main source, 120 s / 1 for its
    others and mid-range fires, 240 / 480 s far; a parcel not on the next band's grid moves to its neighbours' mean over
    its band's last 35% and drops out where it coincides: no pop), leaves from the fire as it was then (the sections,
    injection height, activity and smoke production at that moment: `SmokeEmit`, cached) and is advected by
    `wx::fireSmokeAdvect` (Euler: windAt at ITS OWN position, height and time) on a fixed schedule of ages
    (`fireSmokeStepS`: 0.15 x the age, 20-900 s), cached as `SmokeTraj` (positions at the schedule's ages, interpolated)
    and only extended: a trajectory is a pure function of its emission time (time reversal reads the cache, a jump
    integrates from the emission). Only the head segment (the column's last ~1 min) rides on the wind now. The sections
    are labelled by their bearing round the burnt ellipse's centre from the spread axis (by the cut's start, which
    jumps, a parcel's source index named another stretch of the front at another emission: the streak zig-zagged), and
    an emission cut into fewer sections shares them out by rank. The trail's length is fixed per fire ((3 + 9 size) x
    0.8 h, 2-14 h; it followed the activity and stretched every knot as the day went on), its last segments fade over
    0.8-1 of it. The pall is the mass-weighted mean (sin^2 over [tauMax, tauMax + 36 h]) of parcels on a 4-h grid, each
    carried 2-50 h by the winds it met, its width from their spread. `wx::fireSmokeAt` (harness `fires go view=under`,
    `fires look what=plume`) integrates the same trajectory. Cost: the parcels' extension is a few windAt a frame
    (`fires` -> `smoke_traj_steps_frame`); after a jump ~80 steps a parcel, in parallel on the storm catalog's pool; the
    cache (`smoke_trajectories`, `smoke_emissions`: ~100-400 near four fires) drops entries unused for 600 frames.
  - **Stable sources, pieces shared by what is on screen** (2026-10-27, issue 20261027_212509 "Smoke trails": from 43 km
    over Thailand at 1 h/s, "ultra stretched particles", a star of needles below the fire). (1) The low layer (si 5) and
    the pall (si 6) left from the emission's STRONGEST section (`mainSrc`); on a weeks-old fire (front 126 km, five
    sections of nearly equal heat) that flipped between emissions minutes apart, so the low layer's knots jumped ~25 km
    across the fire (a segment 106-346 s old was 23 km long, ~300 m thick): needles crossing in a star, a new star every
    frame. They now go with the MIDDLE-rank section (`S / 2`: ranks are labelled by bearing, so the same stretch of front
    at every emission), and so does the "main" streak (the fine knot grid, its 0.8 mass with a low layer, `lastXY`). The
    heat-weighted centre was tried: on a long curved front it lies in the burnt area (one round puff). (2) The trail
    pieces are planned in two passes (`SegPlan`): pass 0 measures every segment (in view, pieces wanted: <= 2.2 x its
    width apart, >= 6 px on screen), then the budget (cap less one per pall) goes to every segment IN VIEW (nearest fires
    first), up to a quarter of the rest to segments off screen (their shadow, `smokeSunTransmit`), and the pieces by a
    water fill (the largest count M, <= `kSmokeMaxPieces` 32, whose extra pieces fit). The old share (cap - n - every
    segment left, at most 12) went to 0 whenever the fires in reach had more segments than the cap: every segment one
    Gaussian of sigma 0.5 x its length. `clouds_v2.wildfire_smoke_alloc` 0 (no UI) = the old share (A/B); harness `fires`
    -> `smoke_segments_drawn / _single / smoke_piece_gaussians / smoke_segments_off_view_dropped / _in_view_lost`.
  - **The curtain** (2026-10-28, issue 20261028_192133 "Smoke effects still spawn oversized oval particles": a bright round
    dot led every streak, and the ovals stretched as the trail turned). Each streak Gaussian is built from the smoke's own
    COVARIANCE (`emitCov`): its share of the trail's length along the Catmull-Rom tangent + the section's extent along the
    section's principal axis (`SmokeEmit::ax / extL / extW`, from the heat-weighted points' 2x2 covariance; sigma 1.7 x
    its rms so neighbouring sections join into one curtain along the front) + a round spread + the thickness. It was a
    frame on the segment (width always ACROSS it, `sB0` = 1.2 x the section rms / sqrt S: km-wide ovals that swung with
    every turn, and the head drawn at 0.3 x its length: the young column seen from above as a dense round dot). A
    segment's Gaussians stand on NESTED NODES in u = fixed emission times between its two knots (they age together: u
    does not slide), level s = nodes j / 2^s (0 = the midpoint alone), the next level's nodes fading in with the mass shared
    (the pieces were len / m apart and re-cut at every integer m: a jump). Pass 0's `want` is the node count (2^(s+1)+1).
    `clouds_v2.wildfire_smoke_curtain` 0 (no UI) = the old pieces (A/B). Videos at 1 min/s and 1 h/s from the report:
    harness_runs/fs/sm2 (worktree, untracked). Isolate the smoke from the ground with `clouds_v2.wildfire_scars false`
    (no UI): the report's "blue speckled oval" is the burn scar under the haze.
  - **No smoke Gaussian larger than the curvature of what it stands for** (2026-10-29, issue 20261029_084627 "Haze during
    eclipse": from 11.7 km at the 2037 totality the whole sky was bright white haze streaked toward a vanishing point; from
    79 km a 22-day fire's front was a white arc with straight tangent tails). Three earlier rounds patched sizes; the root
    was single Gaussians far larger than the curve of the shape they represent. Measured with harness `fires` ->
    `smoke_gaussians` (every smoke Gaussian as the GPU gets it: principal sigmas, range, height, the eye's Mahalanobis
    distance, the optical depth through the centre).
    - **Curved Earth** (cloud_march.comp `smokeCurvedInt`): a pall is one flat tangent-plane Gaussian up to 400 x 290 km;
      500 km from its centre that plane is ~20 km above the ground, so near an aircraft the "pall" hung over the eye
      (tau 11 along the view). The density is now evaluated in CURVED coordinates (height above the sphere through the
      centre, tangent-plane offset) by 8-point Gauss-Legendre over the exact intervals where the ray is inside both the
      shell (4.5 marginal vertical sigma; two intervals for a ray that dips through it) and the horizontal footprint (the
      exact horizontal marginal), then carried on as a Gaussian along the ray at the integral's mean. Only where the flat
      form is wrong: Gaussians whose drop over 3 horizontal sigma passes 0.15 of their thickness, and then rays within
      1-2x the sheet's tilt across its footprint (3 sigma_h / R) of grazing, or an eye between the flat and the curved
      sheet; blended (no seam). The per-Gaussian part (marginal vertical sigma, tilt, the eye's weight) is computed on the
      CPU in double (`writeSmokeSide`, dust buffer after the engine lights, `kDustSideSlots`, five vec4 to a slot) and
      packed into shared memory per tile by `cullDustForTile` (`sSmSide`): read per pixel it cost ~0.6 ms. It replaced the
      per-ray "move the flat Gaussian down by the drop" patch (faded out with the eye near the sheet, one crossing only).
      **Register cliff:** the curved branch must leave q, sq and o as the FLAT values and accumulate no second moment:
      any of those held live took the composite from 128 to 249 registers (probes in harness_runs/ecl).
    - **Billows along the path**: their contrast falls as 1 / sqrt(path through the Gaussian / 1.5 x its smallest
      sigma) (the noise read at the closest approach slid tens of km per pixel along a grazing path: the streaks).
    - **Front sections are chains**: `sources` samples the front at 192 points (was 48), each counting its own stretch of
      front (ds^2 / 12 along the tangent), and keeps each section as 4 sub-pieces of equal heat (`SmokeEmit::subOff /
      subW / subC`). The curtain draws a section as 1, 2 or 4 groups along the real front (level from its length over 3 x
      the smoke's round spread and over 12 px, cross-faded), each with 1.7 x its own rms along the front from its 2x2
      COVARIANCE, interpolated between knots as a matrix (no axis to flip or spin). It was one straight Gaussian of 1.7 x
      the section's rms: 50-60 km bars tangent to a curved front. The groups SHARE a segment's pieces with its nodes
      (`want = max(nodes, groups)`): as nodes x groups the plan filled the budget (+25% Gaussians, ~3 ms over a big fire).
    - **Pixel floors at the NEAREST range** (emit / emitCov / the pall's thickness): a pall centred 528 km off but reaching
      the eye was thickened 0.9 -> 3.3 km for its centre's pixels.
    - **Eclipse**: plumes and smoke take the Moon's shadow on their sunlight and its sky light on their sky share, per
      pixel (eclipse.glsl, uniform gate): at totality the smoke stood full-sunlit white.
    - Measured (RTX 3070 Ti, 1920x1009, HEAD vs fix in alternating runs): the report's shots 3.5 / 5.4 -> 3.7 / 5.5 ms,
      over a big fire from 26 km (issue 20261028_192133) 21.9 -> 22.3, beside a fire on the ground 6.0 -> 6.4-6.9 (the
      curved pall where it is needed). Composite 128 registers, no validation messages.
  - **Smoke light**: GREY (2026-10-26, issues 20261026_204045 / _204239 "it appears so beige") — fresh (0.62 grey)
    albedo 0.45 (black carbon) to aged / pall (0.75, 0.76, 0.78) 0.6, the sky's light on it BLUE (0.66, 0.82, 1.08: it
    was a brown-grey (0.80, 0.79, 0.76)), x "Smoke tint" (-1 blue-grey .. +1 brown) and "Smoke brightness"; its scattered
    light is as spectral as its extinction (smoke.glsl `smokeScatterColor`: (1 - e^(-tau sp)) / (1 - e^(-tau)), luminance
    1: a thin veil blue-grey from the side, a thick column grey), replacing the reddening of the light leaving it (0.15 x
    its view depth) that, with the warm tints, made every plume beige. Brown-orange now only where the light crossed
    smoke or air toward a low Sun. The Sun's colour at each Gaussian's centre (`atmExtinctionMag` along its path, the rocket
    plumes' k), and in `cloud_march.comp` the Earth's shadow PER PIXEL for smoke (a 350-km pall lit as at its fire glowed
    over the night side: "glow is stupidly intense", and at dusk "smoke looks super bright"). Composited SPECTRALLY
    (`kSmokeSpec`, Angstrom ~1.6: the Sun seen through it dimmed and red, no sky light given back); the light inside
    reddened by its own path to the Sun (0.6 x, capped 1) — the haboob walls keep their stronger 1 / 3 and their exit
    reddening 0.5 / 1.2 (the first cut's orange-brown smoke). The Angstrom exponent is x "Smoke spectral" (`kSmokeSpec`^s,
    CloudV2Params `gnd.y`, also in `smokeSunTransmit`; 1 = fresh smoke's ~1.6,
    default 0.6: at 1 every cloud seen through a plume turned tan). **Far clouds behind smoke** (issue 20261026_204045:
    "they pock through the smoke layer", light-blue shapes): the air split's pull toward the smoke (cloud_march.comp,
    cloud_v2_combine.comp) used the smoke's light-weighted distance, for a pall often PAST the cloud (the split moved
    farther, the bright horizon air in front of the cloud added undimmed), and split linearly in distance, where the
    horizon air saturates within ~50 km. Now the smoke distance is at most 0.6 of the cloud's, and the split is solved in
    air-brightness space (`smokeAirSplitM`, cloud_occlusion.glsl: air to the smoke + the cloud's share x the air from
    there). Still not spectral (the air between smoke and cloud is not reddened by it). **The air's brightness is the ray's own
    air** (2026-10-27, issue 20261027_182936 "Orange weirdness from orbit"): 1 - e^(-0.2 c(t)), c = the molecular column
    from the eye along THIS ray (`atmColumn`, inverted by bisection; 0.2 c = t / 40 km for a level ray at sea level, the form it
    replaced). With 1 - e^(-t / 40 km) from orbit both ends had saturated (the split clamped at 552 km): every cloud past it
    seen through a trace of smoke (pTau > 1e-3) lost the limb's blue haze in front of it, a hard-edged orange-yellow patch the
    shape of the pall's reach, whatever its optical depth (wildfire_smoke 0.1, spectral 0, brightness 0 all kept it). The
    combine pass takes the camera now (`CloudMarchPC` as push constants). **The sheet's curvature drop fades out with the eye
    in the pall** (within 4 -> 2 vertical sigma of the curved sheet): from 1.1 km inside a 1-km-thick pall a ray along the
    horizon has its flat closest approach ~200 km out, the drop there put its smoke past the horizon's clouds (dustSegW), and
    the bright cloud band showed through as a white line along the horizon (same issue, shot 3). Open: a pall from one big fire
    is tau ~0.4 from above here (the comment at its emit says 0.01-0.05). Billows per Gaussian, a
    second octave at ~4x for smoke. `sat_sky.frag` (`smokeSunTransmit`) dims and reddens the Sun on land and sea under
    it (the first 96 Gaussians; skipped where the Sun is down: it cost ~1 ms from orbit at night).
  - **Smoke behind a cloud takes its own air** (2026-11-08, issues 20261108_171841 "Black line on horizon" and
    20261108_172203 "Render artifacts within rain", inland NSW / QLD, a pall at the horizon). The sky pass has ONE air
    split per pixel; with a cloud on the ray the composite put the smoke at its share IN FRONT of the cloud (pFrontT)
    and, with none in front (gBB ~ 1), skipped the smoke entirely: a pall 200-300 km out behind a thin high veil
    (45-80 km out, seen from 14 km) or behind a rain curtain (a few hundred m out) took the veil's / the rain's split
    and drew dark grey for the air it lacked: a line along the horizon, and the half-res "blocks" in the rain (gone on
    leaving the curtain: no cloud on the ray, the smoke's own distance). Knock-outs: `clouds_v2.wildfires 0`, the
    composite's plume block off, or the sky's split at infinity removed both; the far layer, cirrus, fog, sky TAA, the
    march budget, the Sun volume, every term of the cloud's light did not. Now (`cloud_march.comp`, end of the plume
    block): `dustCloudsCS` also returns `tBack`, the light-weighted distance of the plumes past the cloud's first contact
    (all less those in front: two more sums in its loop took the composite from 128 to 249 registers); the segment
    loop sums the plumes' visible opacity in front of / past that contact; their distances merge in air-brightness space
    (`smokeAirSplitM`, which now takes either order), and the final split weighs the smoke as ALL the composite's
    opacity but the cloud's (1 - A - gBB (1 - T_cloud)). Also with no opacity profile on this frame's ray but a resolved
    cloud nearer than the smoke (rain's sparse rays). DA.a carries the merged distance FLAGGED (the half float's lowest
    mantissa bit, `cloudAlphaFlag`) and `cloud_v2_combine.comp` then splits two-sided with the same weights. The
    brightness is now the air's own optical depth, 0.108 per molecular column (Rayleigh, 550 nm) + 0.0138 per aerosol
    column: t / 40 km for a level ray at sea level as before, half the old 0.2-per-molecular-column above the haze
    (from 14 km the old form put the merged split ~100 km short: a 5-8 level line left). Reporter's views (luminance
    levels): 171841 the line's dip below both sides 26-42 -> <= 4 (no wildfire at all: <= 3); 172203 shot 1 horizon
    band std 8.7 -> 2.8 (no wildfire: 2.7), shot 2 band px > 15 below the median 298 -> 0. Composite 128 registers;
    GPU within contended noise of HEAD with the march unchanged (interleaved, RTX 3070 Ti shared with two other apps).
    Seen changing elsewhere: a pall seen from high above (issue 20261027_004019) +1.4 levels hazier (the slower brightness).
  - **The flame band** (2026-10-27, issue 20261027_212337 "Flame particles": "the sprites seem to disappear when up close
    and are just dots"; `clouds_v2.wildfire_flame_band` 1, no UI, 0 = the old tongues below). Two causes: the close-up
    flames, their share of the points and the glow were gated on the range to the IGNITION point (a fire burning for
    three weeks had its front ~100 km from it: 44 m over that front, 125 km from its ignition, no flame at all; now
    `nearKm`, the nearest of 64 front samples), and the tongues stood >= 40 px apart by the range to the fire's centre,
    each its true size (sub-pixel from a few km, missed by the half-res composite). Now tongues stand `kFlamePx` (9 px)
    apart on screen at the level their own range asks (64 base segments, nearest first, each walked down to 2^16 round;
    levels cross-fade as the points'), budget 256 (smoke's cap is the dust list less that), `flameLodScale` widening the
    spacing when it binds. A tongue the eye resolves (`res` = smoothstep(2, 6) of its height in px) keeps its shape and
    gets two LICKS (narrow, taller, swaying and flickering on their own phases); an unresolved one is its stretch of
    front (a ribbon segment 0.35 x the spacing along it, >= 0.9 px tall) carrying the stretch's flames' light (60% of
    its area at the flame's radiance): no brighter than the flames. Radiance / the on-screen overlap where the front
    runs along the view (flames are near opaque; additive tongues there were a glowing tube). The points keep half
    their flame light within the band's range (their glare / bloom). Measured (RTX 3070 Ti, 1600x900): +0.7-1.4 ms GPU
    500 m from the front. Harness `fires` -> `flame_band`, `flames_wanted`, `flame_lod_scale`,
    `flame_band_top_range_ds_sA_sV_res_K_lv_rad`. Open: the near end of a front running at the eye is still a bright
    glow; the licks are Gaussians, not textured flame.
  - **The front close up, reworked** (2026-10-28, issue 20261028_191813 "Fire effects": "Fountains of Bellagio. Rhythmic
    pillars always spawning in pairs ... more fluttering shape, maybe embers ... the flame front needs to be less of a line";
    and "panning disappearance").
    - **The panning bug**: the front's points and flames were placed over the full DEM at the IGNITION point + the
      18-km copy's change across the fire; 47 km from the ignition that put the front by the eye ~130 m above the drawn
      ground (harness `fires` -> `flame_band_nearest`: el +68 deg at 86 m). The GPU snap moved them down, but the CPU view
      cull had tested the raised place, so looking down at the front beside you every flame and point there was culled.
      Now `groundAbsAt`: the 18-km copy at the point itself, shifted near the eye (3 -> 25 km) to the GPU's ground under
      the eye; the culls allow 25-30 m for what the snap still moves.
    - **A ragged band, not a line**: `wx::fireFrontWiggleM` (four octaves along the arc from the BACK, ~35 m .. 2 km,
      re-drawn over 20 min .. 8 h) moves the edge for the points, the flames and the near smoke (`edgeOf`); each tongue
      stands at a hashed depth into the flaming zone, a third get a smaller flame burning out behind, spot flames ahead
      of the head (8%).
    - **Clusters, not paired licks**: a resolved tongue is 1-4 flames at hashed places along it, each a body + a tip
      leaning downwind and swaying, its height pulsing with smooth value noise in t at the puffing frequency of its
      width (~1.5 / sqrt(D) Hz), the bigger ones shedding a rising, fading puff. Budget 512 (smoke n + 576): two passes
      (`FlameCand`), the nearest tongues take the most flames that still leave every farther one a single flame;
      `flameLodScale` widens the spacing only when even that overflows (`flameNearScale`, the floor 1.4 x the flame
      height, only once that saturates). `kFlamePx` 12. Unresolved ribbons overlap (0.45 x the spacing).
    - **Embers**: kFireBuf points from resolved tongues within 2.5 km (1-4 a tongue, `emberCap` 900 past the points):
      born on a hashed 2.5-7 s cycle, rising on an exponentially slowing updraft, drifting with the 10-m wind, swirling,
      cooling yellow -> red. Their height rides in the direction's length like the points' (the snap).
    - **Smoke at the flames** (`nearSmokeLast`, reserve 80 Gaussians in the smoke's range, fires whose front is within
      8 km): columns ~55 px / 8 flame heights apart, two dark (albedo 0.14-0.26) puffs a column on 10-25 s cycles rising
      a few m/s, widening and fading below the streaks. Not ground-snapped.
    - Measured (RTX 3070 Ti, 1920x1009, the report's place at night 50 m up, FOV 90): GPU 37.25 vs 36.90 ms with the new
      flames + near smoke off (`clouds_v2.wildfire_debug` 9), CPU (the fill, `launches` bucket) 2.04 vs 1.17 ms.
      Harness `fires` -> `embers`, `near_smoke`, `flame_near_scale`, `flame_max_sub`, `flame_band_nearest`,
      `points_nearest_m`; `clouds_v2.wildfire_debug` bits 1 no flames, 2 no points, 4 no smoke, 8 no near smoke, 16 no
      embers, 32 no pall, 64 no front sections across the curtain (one piece at each section's centre) (no UI). Captures
      harness_runs/fs/fn1 (worktree).
  - **Flames close up** (the old tongues, `wildfire_flame_band` 0; within 40 km, faded 25-40 km): emissive Gaussians (dust list `col.w` 1, AFTER the smoke's range
    so `smokeSunTransmit` does not take them for smoke) standing on the active arc, tongues 0.66 x the flame height tall
    and 0.35 x it + 0.05 x their spacing wide (were 0.3 x the spacing: neighbours merged into one band), spaced >= 40 px,
    radiance the flame's pi L / E_sun (~0.5 of a white sunlit surface) x heat^1.5 CAPPED in displayed light at 0.6 (x
    1 / `skyExposure()`: at the night exposure the physical value was a white blob), flickering; at most 96 a frame.
  - **The glow** (the nearest 6 fires at night, within 120 km, faded from 40): up to three engine lights a fire at the
    heat-weighted thirds of the active arc, 30 m + half the flame height up, intensity = the front's summed light (24
    samples) x 0.004, at most 3 suns m^2 a fire (a Falcon 9's flame is ~8, a Super Heavy's ~50), r0 >= 250 m. It was
    0.06 of it at the head (~2000 suns m^2 for a big fire: x the night's ~2600x lift it lit the haze and the ground
    orange over a whole view from 5-100 km: "light is overwhelming"). Now it lights the smoke column's base and the
    ground round the fire, a few km.
  - Settings `clouds_v2.wildfires` / `wildfire_smoke` (x optical depth) / `wildfire_light`, and (2026-10-26) the smoke's
    look: `wildfire_smoke_brightness` 1 (0-5, x its scattered light), `wildfire_smoke_tint` 0 (-1 blue-grey .. +1 brown),
    `wildfire_smoke_spread` 1 (0.25-4, x the plumes' and the pall's width, same mass), `wildfire_smoke_spectral` 0.6 (0-2, x
    the Angstrom exponent ~1.6: 0.6 = ~1, aged smoke's; 0 = grey) (Weather window > Controls > WILDFIRES). Harness `fires` also
    gives `smoke_*` (those settings, the cache sizes) and `smoke_segments_si_a0_a1_len_sb_sc_sigma_tau_z` (every drawn
    segment: 10 x fire index + source, 5 = low layer, 9 = the pall). Harness
    `fires [world=1]` (points, flames, lights, point_lod_scale, point_peak_flare / point_flare_sum, glow_suns_m2,
    smoke_sources, ground_snap; per fire flame height, ground, wind at 10 m / 3 km; the nearest's smoke knots; world=1:
    `world_ignition_on_lights` / `world_front_on_lights`), `fires go [k] [view=side|under|orbit] [local_h=] [dist_km=]
    [back_km=8] [alt=] [pyro=1] [age_h=]` (side: across the SMOKE's wind, back_km upwind of the head; under: below the
    smoke age_h old, `wx::fireSmokeAt`), `fires look [k] [what=head|plume] [age_h=]` (aim at the k-th nearest; run a frame
    after a `fires go`). Views: `harness_runs/fr/views.satcmd` (untracked; captures in screenshots/rework_20261024/fires).
    Cost (RTX 3070 Ti, Medium, 1600x900, 2036-08-12's biggest fire, Madagascar savanna; on vs off interleaved in one run,
    `harness_runs/fr/perf.satcmd`): 600 m from the head at night GPU 12.9 vs 10.4-11.4 ms (+1.5-2.5: the sky pass's
    smoke and scar, cloud_march +0.7-1.1, points +0.24), CPU (the launches bucket, where the fill runs) 1.8 vs 0.27 ms (since perf4-cpu 2026-10-24 the front's 64 base segments are built in parallel on the storm catalog's thread pool, each into its own list, appended in order and cut at the budget — the same points in the same order: `fire.front` 1.7 -> 0.7 ms, the fill 1.95 -> 0.95 ms; a rescan of the fires near a new place, `firesBornRows` over row bands, ~4 -> <1 ms on the landing frame);
    5 km off, 600 m up GPU 8.0-8.2 vs 6.7 (+1.4), CPU 0.81 vs 0.26; from 400 km by day GPU 6.0 vs 5.9, CPU 0.64 vs 0.22.
    cloud_march and the composite at 128 registers. Displayed luminance (mean / p99.9 of the frame, 0-255) before -> after
    at night: 600 m 42 / 101 -> 16 / 130 (the glowing haze gone, the front visible), on the ground 9 km off 30 / 216 ->
    13 / 39, 5 km off 600 m up 33 / 197 -> 13 / 46, 20 km 22 / 127 -> 13 / 43; orbit and day views within 1-3%.
    Not yet: burn scars that outlast the fire, pyrocumulus caps, smoke in the far cloud layer / weather bake, a Fires
    list in the Weather window, fire ambience, fires in a snowy boreal November (the dryness curve leaves ~6% of the
    summer rate at 60 N in late November: the issue's Siberian fires), the smoke's shape by day (a column that bends over
    into a flat layer within a few km; no towering convective column).
- **Haboobs (2026-10-23, reworked 2026-10-24 as CLOUD; `SatelliteSimHaboob.cpp`, `clouds_v2.glsl` `cv2DustWallSigma`,
  `cv2_storms.glsl` `cv2DustWallAt`).** A storm's gust front lifting dust: `haboobFrontOf` makes the front from the storm's
  towers (map frame -> Earth-fixed by the drift phase): line systems along their line of towers, dG ahead (5 km + 14 km x
  smoothstep(0.5 h, 3 h) of the storm's age), curving back round both ends; multicells / supercells a semicircle round the
  dominant tower. **"Ahead" is the storm's motion OVER THE GROUND** (its map-frame motion plus the map's drift): ahead of the
  map-frame motion alone, a storm moving against the drift ran its wall AWAY from a viewer put "ahead" of it (~15 m/s in
  the Gobi tests). Where the biome map's DUST x the front's strength x "Haboobs (x)" (`clouds_v2.haboobs` 1) passes 0.08:
  - **The wall is cloud in the clouds v2 field** (the user: "I really just want the cumulus clouds but beige"). Each frame
    `weatherFillDustGrid` (after `updateStorms`) rasterises the fronts into a 512 x 512 grid in the storm buffer
    (`kDustHeadOffset`, between the cyclones and the envelope index; cells 400 m near the ground, + 0.6% of the eye's
    altitude, to 6 km), on the tangent plane at the observer's sea-level point: per cell the wall's strength e (bits 0-15;
    nothing 1.5 km ahead of the front, full 1 km behind it, the pall behind at half, gone 26 km back; x a smooth noise of
    the distance along the front, 3.5-km knots: a row of HEADS of different sizes, not a slab), its top (bits 16-27, 2 m
    units: 0.9-2.9 km x the heads' noise, 0.4 of it over the pall) and the CLEAR AIR ahead (28-31: from 2.5 km behind the
    foot to ~12 km ahead). Each front point stamps a strip (+-1.2 of its spacing, max-combined), in 16 row bands on worker
    threads; rebuilt when sim time moves 2 s or the eye a quarter cell (the grid's offset from the eye is passed, so a
    stale grid stays in place). `cv2DustWallSigma` is the low layer's 3D margin, m = e + 1.6 lobes - g(z): g least at the
    NOSE (0.35 of the top: the head runs ahead of its foot), the lobes the cumulus shape volume at a 4-km period read at an
    anchor that MOVES WITH THE WALL (the nearest wall's motion over the ground, integrated, + a 2 m/s rise: the billows
    churn up the face); 0.016 /m at the core (visibility ~200 m: from inside, a brown-out). The march lights it as cloud
    (light march, the Sun transmittance volume: the anvil shades it), and tints it: its share of a sample rides as a
    NEGATIVE `CV2Field.soft` (a member more risks the register cliff; every `soft` reader takes max(soft, 0)), and the
    sample's light x `kCv2DustTint` (0.95, 0.70, 0.45), the sky light x its square (scattered many times in a thick wall).
    In the clear air ahead the low cloud and rain go (x (1 - smoothstep(0, 0.8, z)): OUTRIGHT where it is clear; x 0.05 left
    a thin milky veil over the whole view that every ray marched and lit, 7 ms of cloud march at the haboob Watch with the
    user's settings, perf pass 4): the line's shelf band is stamped 1-22 km ahead of its towers whatever the outflow's lead,
    and it and the weather map's rain hid the wall from every Go to. Debug view 7's red includes the dust. **The march
    without the haboob code** (perf pass 4): its presence alone, never run, cost the march 0.5-1.2 ms in every view, so the
    march (and pass B) is built twice, specialization constant 3 `kCv2DustOn` (clouds_v2.glsl under `CV2_DUST_SPEC`), and
    the host binds the dust-free pair while the grid's head holds no wall (`dustGpuOn`, weatherFillDustGrid). Other shaders
    that call cv2Field keep the code (the lightning pass's variant measured nothing). Beyond the grid (from orbit) the plume Gaussians below still draw, faded in over its outer 15%.
  - **The plume Gaussians** (the first cut, now the far field and `clouds_v2.haboob_field` false — "Haboobs as cloud",
    Weather window > Controls): wall, head and pall Gaussians, 1.5-3 km tall, in the aerosol range of the dust list
    (`aerosolFirst`, spectral extinction, no sky light given back), the Earth-fixed billow field (`isWall`). Up close they
    read as 4-20 km flat slabs with an Earth-fixed noise the wall slid through at the storm's speed.
  - **Measured** (RTX 3070 Ti, 1600x900, interleaved in one app): march 128 registers (binary 341 -> 351 KB). Medium, a
    Mongolian MCS from the ground 9 km ahead: cloud march 9.3-9.6 ms; the wall's own cost (it drawn as nothing) 0.1-0.4;
    without the clear air (the rain and shelf left in front of it) 5.2-5.4 — revealing the storm behind costs the other
    ~4 ms. The Gobi card view: 12.1 vs 5.8 haboobs off (High, the rain-only first cut: 30.5 vs 19.4). CPU: the grid
    1.3-1.9 ms a rebuild (every frame in time warp, every 2 s at 1x).
  - Harness `haboob [aloft=1]` = the Weather window's Haboob card (its search on the scout catalog, then its Go to; the old
    search on the MAIN catalog, kept for `kind=` / `k=`, evicted the renderer's bins and stood a fixed 28 km from the
    storm's centre); `fires` -> `dust_grid` {on, walls, cells, cell_m, ms, nearest_wall_km (e > 0.3), e_max}. Scripts
    `harness_runs/hb/` (untracked; `final.satcmd`). Not yet: the wall from orbit is still the Gaussians; the regional dust
    haze (`cv2FogDust`) and the walls are separate; the Sun-lit test for a Go to knows the storm's anvil, not the weather
    map's own overcast.
  **Arcs that fade (2026-10-26, issue 20261026_192211: "when they are done, they just disappear ... pretty huge and
  continuous, I'd expect just an arc front"):** `haboobFrontOf` weights each tower by its strength (smoothstep 0.03-0.4;
  a hard 0.25 cut took the wall with it), scallops a line's front (furthest ahead at each tower, 0.55 of that between
  two), leaves dusty only ~half of it (a 60 / 22-km value noise across the storm's motion, a storm-fixed coordinate) and
  lets the outflow OUTLIVE its towers: the towers of 15 / 30 / 45 min ago (`towersAt`, carried with the storm) sustain
  the current ones' strength, decaying to 0 over 45 min, and stand in alone when the storm has none left (a storm stays
  a candidate while it lives: `stormOrganisedNow`). Mixing lagged towers in as extra sources made a zigzag front (tried).
  **Lower walls, further ahead, the storm left whole (2026-10-26, issue 20261026_204749, 18.1 S 17.3 E: "hyper eroded,
  cavities carved out", "giant chunk missing out of squall line", "does it span all the way up to the cloud base?").**
  The cavities were the CLEAR AIR: it removed all low cloud below 1.35 x the tallest head (~5 km AGL with heads to
  3.4 km) from 2.5 km behind the front to 12 km ahead, and with the front only 5-19 km (cells 3-10) ahead of the towers
  that strip ran through the line's own storm cumulus (it reaches 31 km past a tower's edge): a 5-km tunnel under the
  storm (shot 5; with `clouds_v2.haboobs 0` the cloud is there; debug view 7 shows it is the low layer). Now: walls
  0.5-1.8 km a head (`m.H0` (700 + 500 k) x 0.75-1.25; the far-field Gaussians' H 0.7-1.9 km) — a real haboob is ~1-2 km,
  rarely ~3, under a dry-desert Cb base 2.5-4 km up — so the clear air is a BASE LIFT to ~3 km AGL (its edge lumps
  1200 m x the coarse lobes, was 2400), and it reaches back over the pall to 8 km behind the front (the wall's top was
  inside cloud there); the front leads a line by 8 + 22 km x the age ramp (~30 km mature, in the line's subsidence
  ahead of the shelf band) and a cell by 4 + 11 km. Harness `haboob aloft=1` stands 2 km up (2.5 was now inside the
  cloud above the walls). **The clipmap is rebuilt when the dust field changes** (`cv2CdcDustKeyUsed`: dust on / off,
  gain, form): cells baked empty under the clear air kept skipping the cloud after the march swapped to the dust-free
  specialisation, never expiring with time paused (rectangles of missing cloud, popping in as cells expired — the
  likely "lighting snaps near haboobs" of 20261026_191842, whose trail did not show a snap on or off afterwards). The
  shot-6 "eroded top layer" is the ANVIL shields (`clouds_v2.anvil 0` removes it; storm erosion, the mid and high
  layers do not), and its pop-in after a turn is the resolve converging that thin, frayed ice: Sun volume, clipmap,
  S2 light and the faint-sample skip off each leave the convergence unchanged (harness_runs/hb26, untracked).
- **The Weather window (2026-10-23, `SatelliteSimWeatherWindow.cpp`, `shaders/weather_radar.comp`; the weather-cloud
  button beside Missions on the right HUD panel (`TimeWeatherBtn`, `pixel--weather.png` from `tools/make_icons.py`), key N
  `KB_WEATHER`, window id 12, `wxChrome`; harness `ui open weather [tab=radar|storms|controls]`, `weather ...`).** Missions-
  like, for the storm objects. **Opens on Storms** (2026-10-28, issue 20261028_222628), drawn first: Storms, Radar, Cyclones,
  Controls (`wxTab` keeps its values: 0 radar, 1 storms (default), 2 controls, 3 cyclones; not persisted). **Radar**: a local precipitation radar centred on the observer (azimuthal equidistant, north
  up, radius 50 / 150 / 400 km, 256 x 256 cells), NWS reflectivity colours (rain green .. yellow .. red .. magenta, snow
  blues and mixed pinks by the rain particles' air temperature 500 m over each cell's ground), range rings, you, storm
  markers (strongest tower of each storm; the label's number is its place in the Storms list; click = its row with Go to /
  Watch), tornadoes on the ground (2026-11-08, issue 20261108_173628: a red down-pointing triangle with its point on the
  funnel, from `tornadoOf` for each storm marked on the map; waterspouts only over water; harness `weather radar` ->
  `radar.tornadoes_on_map`, `radar.tornado_marks` [{storm, lat, lon}]), the low cloud's cover faintly, and a Loop over the frames taken while it was open (one per 5 min of sim time,
  10 kept; a move or a range change starts it again). **The rain is the renderer's**: the pass binds the cloud march's
  descriptor set (set 0, so it sees exactly the weather cube, flow, drift, morphology and `stormBuf` the march does; set 1
  = its host-visible output) and calls `cv2Field` at 120 m over `cv2Ground` in every cell, like the lightning pass's rain
  map at the eye: the map's precipitation through cv2FieldLow's flowed lookup and the storms' rain cores (`gCv2RainCore`,
  set by cv2ColumnSigma inside cv2Field). Its unit is the rain's extinction / (0.0012 x "Rain" `cv2.rain.x`), so ~1 = the
  map's heavy rain, ~2 under a dominant tower's core; dBZ = 23 + 16 log10(60 rr^1.5) (Marshall-Palmer on the particles'
  rate). A sample HIGHER over the ground (the lightning pass takes one 600 m below an in-cloud eye) was tried and drew rain
  the ground never gets; at 250 m the rain floor thickening toward a low base (`kCv2RainIntoM`) saturated whole tropical
  regions at 50 dBZ. Dispatched only while the radar is shown, after `recordCloudsV2` (not on Potato: no cloud pass), at
  most 4 times a second and only when the time, the place (> a quarter cell) or the range changed; read back the next
  frame it finishes (`weatherRadarReadback`, gated on `completedFrame()`) and coloured on the CPU into an R8G8B8A8_SRGB image uploaded like
  the Kessler tracker's globe (`weatherRadarRecordUpload`; the UI's 5th external image slot of 6). Agreement, checked by
  standing at radar cells (`tools/harness/radar_agree.py`: the radar's 4 centre cells x "Rain" vs `state` ->
  `clouds_v2.rain.rate_eye`): at 10 points round a supercell from 120 m, 0 / 0, 1 / 1 (both saturated), 0.69 / 0.61, 0.67 / 0.60, 0.33 / 0.23, 0.12 / 0.16, 0.05 / 0.02; the rest is the radar's 0.8-km box and footprint-averaged shaft against the eye's 80 m). Cost (RTX 3070 Ti, 1600x900): 0.09-0.11 ms GPU a dispatch; the read back + raster 1.8-2.8 ms on the frame after one (<= 4 a second); `build_ui` +0.11 ms averaged with the radar running at 1x; closed, nothing. **Storms**: near you (1200 km, nearest first, up to 12) and the strongest worldwide
  (organised only unless "Include single cells"), from `wx::Catalog::stormsAt` (rows copied: its pointers die on the next
  call; at most twice a second): kind (an unknown `StormKind` is "Storm"; every loop goes to `towerCount`), stage from
  the strongest tower's lifecycle (forming / growing / mature / decaying), age, time left, distance / bearing, strength,
  flashes a minute ESTIMATED as the lightning pass does (rate x smoothstep(0.35, 0.75, strength) of each dominant
  tower). Go to = the harness `storms go`'s placement (1.5 km ASL, 50 km upwind of its strongest tower against the
  anvil's shear, looking at it 8 deg up); Watch = its mature moment (dominant tower grown + 30 min, before it decays) at
  1x, then Go to ("Replay" once it is past). **Controls**: storm objects, frequency, lifetime, cloud field, moat, both
  lightning rates (own rows: the settings' slider rows are tied to the settings window) + "All weather settings".
  **Storm milestones (2026-10-23):** a Storms-tab row shows a supercell's tornado (touchdown in / on the ground / ago, from
  `tornadoOf`) and when a line system's convective line reaches the observer where they stand (`weatherLinePassage`: the
  observer's distance ahead of the line less the towers' bow, through zero within the line's length; 5-min steps over its
  convective life, bisected), as text (the row's buttons Tornado / Passage are gone: see ONE BUTTON below; `weatherWatchTornado`
  and `weatherWatchPassage` remain for the harness). Harness
  `weather tornado|passage [k] [list=world]` (no k: the first listed storm with one ahead); `weather state` rows carry
  `tornado_in_min`, `tornado_min`, `passage_in_min`.
  **ONE BUTTON, Watch (2026-10-24; the user: "people want to just see the event and thats it"; "1 minute before touchdown
  type event calls ... confuse playtesters into teleporting to a place with nothing going on (yet)"; they could not tell
  Go to from Watch, nor the other actions):** every card, NEAR YOU row, radar selection, cyclone row, season row and
  milestone in the Weather window has exactly one button, Watch, which sets the clock so the event is ALREADY happening
  (never before it starts; forward to it when it is still to come), 1x, unpaused, and puts it in frame; the toast says
  what you are looking at. Tornado / waterspout (`weatherWatchTornadoOf` mode 0): the funnel on the ground now if it is,
  else 30% into its life, from the ground ~4.5 km off (40 m up, 7 deg), then moved out of the rain by the radar probe
  (`weatherTornadoFixTick`: rings 4-8.5 km). A storm (`weatherWatchStormNow`, the supercell / line cards, NEAR YOU,
  the radar's selection, harness `weather watch`): its tornado if one is on the ground now, else the storm at its mature
  moment (`tMature`, forward only), from the ground (300 m up) 35 km (lines 30 km) upwind of its strongest tower, 10 deg
  up, then the radar probe at 150 km (rings 25-46 km, `WxTornFix::rangeIdx`). Hurricane: the strongest now from orbit
  (Orbit framing). Haboob: 6 km ahead of the wall's dustiest front-lit stretch on the ground (was 9 km). Wildfire: beside
  the column. Cyclone rows: from orbit now; season rows (a click on the row shows its track): now if alive, else at its
  peak; milestones: that moment, a LANDFALL at it from just off the coast (was 2 h before; the offshore step goes toward
  where it came from). The other views stay in the harness (`weather event <kind> act=1..3`, `weather go`, `weather
  tornado|passage`, `storms ...`). Not judged: whether 35 km frames every storm well (a long squall line overflows the view).
  **Featured events (2026-10-23, `SatelliteSimWeatherFeatured.cpp`; the user: "anytime a new storm appears the window's
  extents change ... reserved spots, like the twilight one in the Missions tab; always a tornado, haboob ready to go to;
  cool events over storms near you"):** the Storms tab opens with EVENTS, one fixed-height card per kind, always in this
  order: Tornado (the next touchdown anywhere, 24 h ahead, ranked daylight 8 + over land 4 + a drier region 2 (the
  map's cover < 0.45 round it) + outside the deep tropics 1, then the earliest — the first cut stood in a downpour over
  Lake Victoria), Hurricane (the strongest cyclone now; its text names the next one peaking at Category 3+, any basin), Haboob (the next line system
  at full strength by day whose LEADING EDGE (9 points along the line, dG ahead as haboobFrontOf) is mostly over the
  biome map's dust (>= 6 at >= 0.3, mean >= 0.4), 48 h ahead — the centre's dust alone picked storms whose front ran over
  farmland, and no wall was drawn; inland (150 km from the sea: the coast's fog); ranked by its light: the Sun on the
  side it advances toward, clear of the anvil (`haboobSunOnFace`) and 18 deg up 2, the Sun 25-60 deg up 1; placed by
  `haboobStrongestPoint`: the front's dustiest, best front-lit stretch at that moment, looking at it — they stood a fixed distance from the storm's centre, 40-80 km from the wall actually drawn), Supercell and Squall line (the strongest alive now, mature and daylit preferred), Waterspout (as the tornado, at sea), Wildfire (the biggest fire burning now, by size x
  activity: 4 km beside its front across the smoke's wind and 1.5 km upwind, 150 m up, on the side with the Sun behind you by day). **Tornado views leave the rain by radar** (2026-10-24, `weatherTornadoFixTick`): ten frames after the jump (the weather cube re-baked) one radar dispatch at 50 km (`wxRadarProbe`, read into `wxProbeFrame`, not shown) scores 48 spots round the funnel (12 bearings x 4.5-12 km) by the rain there x 1.5 + along the line of sight, and moves the eye to the driest (log: `weather: tornado view moved out of the rain`); a CPU estimate from towers and shields stood in full rain (the map's rain and the envelope decks are only on the GPU). Each card is an icon, three unwrapped lines clipped at its edge and one Watch
  button (disabled when there is nothing), so nothing moves as storms come and go; NEAR YOU below is 4 STICKY slots
  of organised storms (a storm keeps its slot while it is near; air-mass cells only when nothing organised is near), the
  rows fixed-height too. **Never search ahead on the main catalog:** `stormsAt(t)` evicts every bin outside [t - 9 h,
  t + 2 h], i.e. the bins the renderer needs this frame. The forward search runs on a SECOND catalog (`wxScout`, the main
  one's Env / Settings / climatology, copied where it is configured in updateStorms; reconfigured on a new
  `stormEnvKey`), 30-min steps, ON A WORKER while the window is open (2026-10-24, `wxScanJob` / `weatherScanStep`: a job
  takes a copy of the search's state, runs steps for up to 0.4 s or until done, and the main thread takes its result;
  only that job touches the scout, so a reconfigure waits for it; on the main thread with a 2.5-ms budget a single step
  was up to ~12 ms — the scout's new bins after a jump — and the harness `weather events` waits on it,
  `weatherFeaturedFinish`); it restarts (cancelling a running job, its result dropped) when sim time goes back before
  its start, moves 12 h on, or a found event is over. A storm born after step k cannot touch down before step k's
  time, so a daylit tornado found before the step reached ends that slot's search. The fires come from `firesWorld` on a
  worker (std::async; cleanup waits for it: biomeMap is destroyed before the future). Storm icons (`tools/make_icons.py`
  `storm-*`, icon indices 23-30): tornado, hurricane, haboob, supercell, squall, waterspout, fire, cell. Harness `weather
  events` (the search run to its end), `weather event <tornado|hurricane|haboob|supercell|line|waterspout|wildfire>
  [act=0-3]`, `weather state` -> `events`, `near_slots`.
  **Named storms (2026-10-23, `SatelliteSimWeatherCyclones.cpp`; the Cyclones tab, `wxTab` 3, drawn third):** the
  tropical cyclones as copies of `wx::Cyclone` (`wxCycActive` from `stormCyclonesNow`, `wxCycSeason` = one basin's
  systems from `cycloneSeason` for last / this / next year within ~200 days of now, `wxCycNext` = the next genesis per
  basin; copies because the catalog's seasons can be rebuilt under a pointer; refreshed every 2 s, after an hour's jump or
  a new `stormEnvKey`, only while the window is open). A TRACK MAP: an equirect land / sea raster (512 x 256, the UI's
  6th external image; `kMaxExternalImages` is 8 now), the selected storm's track box (2:1, >= 40 deg) or the world,
  re-rastered only when the box changes (~2-3 ms), with every active track and the selected one as overlay dots in the
  NHC Saffir-Simpson colours (bright so far, faint ahead: the whole track is known) and the centres as rings (click
  selects). The radar draws the active tracks in range the same way and each centre with its name (the eyewall towers
  are no storm markers); the eyewall ring, bands and eye come out of cv2Field on their own. Rows: category in the basin's
  term (`cycloneCategoryName`), vmax (m/s, kt), pressure (the catalog's Atkinson-Holliday), motion, formed when, over
  land, distance; Watch (from orbit: over the eye at 1.6 x its outer radius, 0.7-2.5 Mm, looking down). `weatherGoToCyclone`
  keeps the other framings for the harness / featured acts (0 near: 60 km behind its motion at 1.5 km, as `storms go
  cyclone=`; 2 Eye: 2.5 km up at the centre; 3 Landfall: its next (else first) landfall, standing just OFF the coast there
  — stepped out until open sea, 20 m up — looking at it).
  MILESTONES (`weatherCycloneMilestones`, from the hourly vmax / overLand): genesis, named (17.5 m/s), hurricane / typhoon
  / cyclone strength (33), rapid intensification's onset (+15.4 m/s in 24 h), Category 3 (50), peak, every landfall
  (`overLand` 0 -> 1), dissipation; each milestone's Watch jumps the clock there at 1x with the camera over the storm from orbit (a
  landfall: AT it, from just off the coast looking at it). Squall lines / MCSs are listed in the Storms tab with
  their kind names; their stage is the regenerating envelope's (`sysOn` / `sysOff`), each slot's strength from
  `towerCycleAt`. Cost: closed nothing; open on the Cyclones tab `build_ui` +0.17 ms (layout of the rows), the map raster
  only on a box change.

## Lightning, thunder and weather evolution (from "Subsystem: Clouds v2")

- **Lightning + thunder (rebuilt 2026-10-05; `cloud_v2_lightning.comp`, `SatelliteSimLightning.cpp`,
  `lightning.vert/.frag`, `include/lightning_draw.glsl`):** WHICH flashes happen is the GPU's: one workgroup walks two
  lattices around the observer out to the cloud tops' horizon — the Cb TOWERS (dominant, anvil-reaching: "Lightning
  (flashes/min/tower)" `lightning_rate`) and STORM REGIONS over every deep-convective, raining region of the weather
  cube ("Lightning rate: storms (/min per 20 km)" `lightning_storm_rate_per_area` 0.5, slot 267, `cv2.lightning2.x`) —
  so whole storms flash, not only their towers. The regions are a FIXED 64 x 64 equal-angle lattice on all six cube
  faces (~156 km, the same at any eye altitude: the first cut's cells grew with altitude, so descending with time
  paused reshuffled every flash); each draws candidates at the full rate at random points and keeps each with the
  storm's activity there (thinning: only live candidates read the weather). **Every live flash probes its column**
  (16 `cv2Field` samples, ground to tropopause + 1.5 km, cloud without rain or the thin high layer > 4e-4/m) and takes
  the DRAWN cloud's base and top; none, or under 1.5 km deep, and it does not flash — the Cb type's heights put
  flashes in clear air above the capped cumulus away from towers (user snap 1). Measured: snap 1 (9.9 km) 27 flashes
  a minute out to ~360 km; snap 2 (609 km) ~10/s over the visible disc. Deterministic in sim time (`cv2FlashIntensity`: a 50-ms stepped leader, then
  1-4 return strokes ~35 ms over a continuing current; in-cloud flashes 2-6 softer pulses). Record (`CV2Flash`, 64 B,
  `kCv2FlashMax` 128): origin, ground point, KIND (0 in cloud, 1 cloud-to-ground ~30% where strong, 2 red sprite, 3
  spider lightning just under the base), seed, age, the cloud's base/top, strength, duration. Ground strokes leave the
  cloud's flank and land 1.5-9 km aside (slanted). HOW they look is the host's: `updateLightningBolts` reads the
  previous frame's list (only a list the pass wrote last frame: `cv2LightningObsDirValid`, one-shot, with the observer
  direction it was written in) and builds each new flash's CHANNEL once as geometry (`buildBoltTree`, a fractal tree
  from its seed: midpoint displacement down to ~3 px at its distance; a ground stroke's in-cloud wander, exit and long
  slanted descent, branches every ~260 m / "Lightning tendrils" (`lightning_tendrils`, slot 268), twigs and many dim
  tendrils, upward streamers at the strike point; a spider's 3-6 near-level arms 6-30 km with a web of twigs and
  drooping tendrils; a sprite's 20-45 forking tendrils and rising streamers), cached by id, and uploads the flashes'
  frames + state (`GpuBoltFlash`: revealed arc — the leader working down, a spider spreading at 150 km/s — the
  branches' share, which fades ~0.12 s after the first stroke, 4 cloud-glow emitters) and the segments (`GpuBoltSeg`,
  32 B, up to 128k). Drawn in the main pass AFTER the sky TAA at full resolution, SCREEN-blended (src + dst(1 - src):
  exactly the sky tonemap 1 - exp(-x) of the summed light, so overlaps saturate instead of clipping): channels as
  antialiased capsules at their TRUE luminous width (3 m main, 1.2 / 0.6 / 0.35 m; sprites 120-380 m) — thinner than a
  pixel they are a pixel wide with their energy kept, never fatter — plus a halo (light scattered by rain and air: 70 m
  main, 22 m branches), hidden by the cloud composite in front (`cloudPointVisibilityAt`; rain shafts included), faded by
  the air COLUMN between eye and point (`boltAir`: a flat haze length left ~5% from orbit); then the cloud lit around
  the flashes in ONE full-screen pass looping over the emitters (per texel of the half-res composite, `boltGlow`: a
  ~4-km diffusion core floored at ~3.5 km and a faint 15-km tail, windowed to 45 km — from orbit a flash is a soft
  10-20 km patch). The lightning pass's thread 1 adds the flashes' light to the rain at the eye (`cv2RainAmb`): drops
  light up in a close strike. **Thunder is streamed from sound shells (2026-10-10, `addThunderSource` / `thunderStep`,
  SatelliteSimAmbience.cpp):** each flash's channel is kept as a source (pieces in Earth-fixed axes, 48 flashes, 150 s);
  its sound travels at 343 m/s in sim time, and each frame the pieces whose g = distance - 343 (t - t0) changed sign
  between the last frame's listener/time and this one's are what arrives: level = 270 x sqrt(length x weight / r^2 per
  second) per flash, with mean distance and pan, pushed to one of the synth's 8 voices (`pushEvent`, [voice, level, km,
  pan]; pink + brown noise, cutoff 5000 / (1 + km / 1.5) Hz, crackle within 4 km, a shared ~2-s rumble). So it follows
  the listener (toward a storm: sooner, compressed at most 3x; outrun: never; paused: nothing; a > 5 km or > 20 s jump:
  silent) and time warp. Driver `thunder` (arriving within ~2 s, sounding, or within 4 s after) gates the layer, so it
  is listed only when there is thunder. It replaced an envelope precomputed at the flash for the listener's position
  then (`queueThunder`), whose brown noise under a 100-360 Hz cutoff was sub-bass only and whose layer was always
  "playing" (user: "displays as playing, but I don't really hear it"; at that snapshot the nearest flash was 27 km off).
  `audio record` advances the shells on its own clock (`thunderRecordTick`). Harness: `lightning` (flashes with kind / segments, `trees`, drawn counts,
  thunder queue), `lightning spawn kind=cg|ic|spider|sprite dist_km= az= [seed=] [base_m=] [top_m=] [ground_km=]`
  (heights above the eye's ground; injected flashes run the same channels, glow and thunder), scripts in
  harness_runs/lightning (spawn2: a clear desert night; t5: orbit + aircraft at 5 N 90 W). Cost ~0.3 ms for the
  lightning draw. It REPLACED (2026-09-29 .. 10-04) `lightningCS` / `spriteCS` in cloud_march.comp: a 14-vertex channel
  with fixed branches and the glow, per pixel inside the half-res composite (smeared by the sky TAA, a 3-m minimum
  width = pudgy), and only dominant towers flashed (a map-typed storm without anvil towers had none: user snapshot 43).
- **Weather evolution (2026-09-29, decision A):** the weather cube is re-baked as sim time moves
  (`SatelliteSim::recordWeatherEvolution`, one face per frame with all its mips, once it lags by ~300 m of
  wind drift or the settings change; the bake's pipeline and per-mip sets are kept, `cv2Wx*`). **In time warp all six
  faces are baked together** (2026-10-23, issue 20261023_224032 "Weather system updates choppy", 1 h/s from 3300 km): once
  six frames of sim time pass the face's `due` (45 s with storms stamped), or every face is stale, one dispatch with z = 6
  bakes the whole cube at ONE sim time every frame. One face a frame left each face up to six frames (~13 min at 1 h/s)
  behind, every face at its own moment: the storms' stamped cloud stepped face by face; and past a 15-min step
  updateStorms makes every face stale each frame and the worst-first pick took face 0 every time, freezing the other five
  until the warp stopped. GPU within noise at 1 h/s from orbit (29.95 vs 29.88 ms). `clouds_v2.weather_one_face` (no UI)
  restores the old schedule (A/B); harness `state` -> `weather.cube` (rebakes, all_six_bakes, face_lag_s, tick_s, and
  since 2026-10-25 the bake's own GPU timestamps: bake_ms, bake_faces, bake_ms_per_face; since 2026-10-27 rect_bake,
  rect_bakes, env_job_ms). **In time warp a face in view bakes its fine levels only over the rectangle the eye can see**
  (2026-10-27, `weatherViewRect`; GPU Performance Profiling "Weather perf pass 2").
  **Resolution and tick (2026-10-25, the user: overlooking a hurricane from space, "the square cells update and move in a
  blocky manner", clouds jumping):**
  - **2048 faces, ~5 km** (`kCv2WeatherFace`, 9 mips): the finest level matches the 8K source map. Every OTHER read names
    the LOGICAL mip of the 1024 cube it was written against, `CV2_WX_LOD(l)` = l + 1 (clouds_v2.glsl; also the march's two
    mip-2 reads, the radar, sat_sky.frag's flat layers at +1, `cv2WeatherBilinear` and the ground shadow's `gCv2WxLod`),
    so only `cv2WeatherSmooth`'s fine read (the low cloud's placement) gained resolution; types, rain, ground, the far
    field's fraction and every coarse lookup keep their scale. The bake's push constants: `srcLod` = the mip (a 2048
    texel is a source texel), `remap.w` = the logical mip (levels 0 and 1 untouched, the coarser ones true mean coverage),
    `pad` = 2 at level 0 (the convective-variance ring at the radius it was tuned at). **Level 0 computes the COVERAGE and
    the PRECIPITATION** (the map's 2x2 taps through both advected copies, the storm stamp, growth, the afternoon term with
    a ~40-km read for its local mean; the precipitation from that coverage, the variance ring read only where the cloud is
    deep enough to rain) and takes type and ground from level 1 (`wxLevel1`, bake binding 5, a sampled 2D-array view of
    mip 1 in GENERAL: levels 1..8 are dispatched first, a barrier, then 0; `recordWeatherBakeDispatches`). The full
    computation at 2048 was 1.85 ms a face; the precipitation from level 1 (upsampled) rained over every texel the 10-km
    field touched: +4.5 ms of march in the matrix's storm cell (11.2 -> 15.5 ms), type and ground from it cost nothing.
  - **The storm stamp is averaged over the texel where a cyclone reaches** (`stormStampTowers` once at the centre, the
    cyclone part `stormStampFinish` at 3 x 3 points: a hurricane's arms, filaments and eye edge are a few km sharp, and
    point-sampled at the texel centre they switched whole texels on and off as the storm moved ~45 m/s through the map's
    frame — the drift plus its track). The towers' and lines' fields are smooth over tens of km: supersampling them over
    the tropics' storm envelopes cost ~1 ms a 2048 face.
  - **Tick**: with storms stamped a face is due once "Weather cube tick (s)" behind (`clouds_v2.weather_tick_s` 10, no UI;
    was a fixed 45 s: every re-bake stepped a hurricane's arms ~2 km). 10 s is <= ~0.5 km of a hurricane's motion, a tenth
    of a texel. Faces out of view (the cap the eye sees, acos(R / (R + h)), + 54.7 deg of a face's half-width + 6 deg) are
    due at a quarter of the rate. Outside time warp a due face is baked over `kCv2WxSlices` (4) frames of rows (`wx.z` =
    the first row; ~0.13-0.32 ms a slice). In time warp (`6 x frameDt > due`) the faces go in one dispatch with a FACE MASK
    (`evo1.w`'s bits; the dispatch's z is the n-th set face) and a quota of ceil(faces in view x frameDt / tick) a frame,
    the most behind first, in view first, one out of view when the quota leaves room; all six after a settings change or
    a jump. With time held (paused, `wait settle`) every face is re-baked when the storms uploaded change
    (`cv2WxStormSig`: tower / shield / cyclone counts): a cube baked on a jump's frame before the catalog had that moment's
    storms kept a hurricane without its arms until time ran.
  - Measured (RTX 3070 Ti, 1600x900, the user's settings, Hurricane Sara from 950 km): march 16.13 vs 16.22 ms (1024),
    128 registers; bake ~0.7-1.3 ms a face (level 0 ~0.4 + stamp, level 1 ~0.27, coarser ~0.2); 1x: a 0.13-0.3 ms slice
    on ~5% of frames; 1 min/s: slices, ~0.4 ms a frame; 5 min/s: 2 faces a frame (~2.6 ms); 1 h/s: 4 faces (~5 ms, was
    all six at 1024 ~4.2 ms); GEO: 6. Frame-to-frame change at 10x (p99.9 of a capture pair's difference): the re-bake
    spikes (11-15 levels every 45 s) gone (3-4 uniform, the rest lightning); 5 min/s median 32 -> 21. Tried: a cubic
    B-spline read of the cube (`-DCV2_WX_BSPLINE`, four bilinear fetches: smooth outlines, +0.3 ms, but it thins the
    narrow arms at 10 km; left as a compile-time option). The bake A/Bs in one app: harness `shaders reload weather=<spv>`.
  The bake
  (`cloud_v2_weather.comp`) reads the map through TWO copies advected by a slow curl wind (`evoWind`,
  ~4000 km, turning over a day), each over its own window P offset by P/2 and weighted sin^2 of its phase
  (0 at its reset): the flow-map double-phase trick, so the map moves locally and never drifts from itself
  (displacement <= wind x P); and its coverage grows and decays under a smooth field (`evoGrowth`, ~1500 km,
  10-30 h), so the types it classifies — storms included — strengthen and weaken. Everything downstream
  (towers, anvils, lightning) follows. Settings: "Weather evolution wind (m/s)" `evo_wind_mps` 8 (0 =
  the static map, exactly as before), "Weather growth / decay" `evo_growth` 0.12, "Weather evolution
  window (h)" `evo_window_h` 3 (slots 182-184). Cost: a face bake is ~0.1 ms (not measurable at 1 h/s).
  With it on, a snapshot's moment shows the map moved up to ~86 km and re-shaded: an older snapshot frames
  a slightly different storm. The rigid drift (`cloudDriftPhase`) is unchanged and on top.
  **Afternoon land convection** (`evo2`, "Afternoon land convection" `evo_diurnal` 0.6, slot 185): the
  bake raises coverage (+0.2) and pushes the type toward congestus / Cb (+0.3) over land where the Sun of
  THREE HOURS AGO stood high (the ground heats through the afternoon; `weatherEvoPC` turns the sim's
  Sun back 45 deg and into the map frame) and the map has cloud nearby (its moisture proxy): continental
  afternoon storms, dying back after dark — the one-moment map carries no diurnal cycle of its own.
  **The flat stand-ins read the same cube**: the sky set's binding 7 is the weather cube (a `samplerCube`
  written by `writeCloudsV2ConsumerDescriptors`), not the 8K equirect map, so `sat_sky.frag`'s flat layers
  (the mirrors' and env probes' clouds, SKY_LITE) and Potato's deck show the evolving map the volumetric
  clouds are built from (the Earth-fixed direction turned by the layer's drift into the cube's frame, one
  mip coarser than the equirect's). The mesh shaders' `earth_env.glsl` fallback keeps the 2D map.
