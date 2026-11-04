# Kessler syndrome: The Sun's Fury

The scripted storm, dead satellites, debris pool, engine, dust, re-entries, rollback, prebake, memory.

Part of the subsystem notes (`docs/subsystems/`, map in `AGENTS.md` §4). Moved verbatim from
`CLAUDE.md` on 2026-11-03. Do not read this file whole: `python tools/outline.py docs/subsystems/kessler.md`
lists its sections and topics with line numbers. New notes go next to the section they belong to.

## Subsystem: Kessler syndrome — "The Sun's Fury" (2026-10-16, branch kessler)

Plan, design log and measurements: `.plans/KESSLER_PLAN.md`. Code: `Kessler.h/.cpp` (app-independent: orbit
maths, the breakup model, the engine, the deterministic `kessler::World`), `SatelliteSimKessler.cpp` (the sim
side: worker thread, mirror, GPU uploads, cache, UI, labels), `shaders/sat_orbit.comp` (pool records, dead
satellites), `shaders/sat_rebake.comp`, `SpaceWeather.cpp` (the scripted storm).

**It is not a mode: it happens in sim time.** A Carrington-class CME (flare 2037-07-14 09:30 UTC, arrival
2037-07-15 03:00 UTC = the scenario EPOCH, Kp ~11 for ~half a day; `spaceWeatherSetScriptedStorm`, the aurora
follows with no other code) kills a share of the satellites (by altitude and inclination, crewed stations 10x
less; hash-chosen, `stormSeedEvents`); dead formation members bump their neighbours (lognormal median 4 days,
each bump kills with p 1/2 and may chain); then the cube-method engine carries the collisions forward. **Before
the epoch nothing differs from the plain sky** (every term is gated on the scenario's seconds). Settings > Launches &
debris > THE SUN'S FURY: the toggle (`kessler.enabled`, default on), collision rate (`rate_multiplier`,
x1 = physical), avoidance (`avoidance`, 0.9), horizon (`horizon_years`, 20), dust cloud brightness
(`dust_gain`, x100), "Watch the next re-entry", live counts.
Default bookmark "The Sun's Fury" (Monument Valley, 06:30 UTC on the storm night, 1x).

- **Everything is a pure function of sim time:** an append-only EVENT LOG (`kessler::Event`: kill, bump,
  collision, explosion, ASAT; origin engine / seed / user) with born / dead / end SECONDS since the epoch. Going
  back before an event just stops drawing its debris and brings the satellites back. Random numbers come from
  hashes of (seed, ids), never a shared stream, so results do not depend on threads or order. **Bump
  `kCacheVersion` (SatelliteSimKessler.cpp) whenever the world's logic changes**: the cache key does not see code.
- **Dead satellites stay roster records** (kesslerBuf words 2i / 2i+1 = dead second, end second; since 2026-10-20 the
  deployment era's born / gone words follow from 2N: CLAUDE.md "Launches" > The deployment era): they keep
  their orbit and TUMBLE (`deadTumble`: an integer period 40-600 s and a phase from the index, the angle taken mod
  one period from the scenario's whole seconds, exact at any date; CPU mirror in `orbitElemsOf`, which therefore
  depends on the current sim time for roster satellites), never manoeuvre (engine: passive), and drag sets their
  end (re-entry). The first cut made each dead satellite a pool record: the pool passed 2M records by day 120.
  **A dead formation member drifts out of its ring** (round 2): no station keeping, so along-track by 0.5 k tau^2
  (k = +-1..3 x 2^-47 turns/s^2 from the index: ~1-4 km after a day, ~100-400 km after ten), taken mod one turn in
  exact integer arithmetic (`formationDrift` in sat_orbit.comp with `umulExtended`, `kessler::formationDriftTurns`
  on the CPU, also in `World::stateOf`; GPU vs CPU ~1 m).
- **Only what can be seen is a record (round 2, the user: "optimise for what is visible").** Pool records are the
  LARGE pieces, Lc >= 1 m (`WorldSettings::LcRecord`), weight 1 (beyond `maxRecordsPerEvent` 96, tapering as the
  750k pool fills, each stands for several); the largest is 0.15 x the larger object's bounding diameter (<= 25 m:
  a 72-m Starmind leaves ~11-m pieces; it was <= 4 m for everything), and their mass never exceeds 60% of the
  parents'. When a satellite over 20 m shatters, its **appendages shear off whole** (`addStructuralPieces`: 2-6
  array sections and 1-2 radiator sections of up to 0.15 x its size, 10-50 m/s), which the size distribution
  (made for compact bodies) leaves out. Each piece is drawn with the **debris kit** (`tools/make_debris_models.py`
  -> `data/satellite_models/debris_{panel,radiator,chunk}_{s,m,l}.json`: a torn, folded array section with a spar
  stub; a radiator section, white paint + OSR tiles, with its manifold; a piece of bus with MLI and fittings),
  appended as types at load (`kDebrisKit`, 16-lobe budget). The kind follows the parent's surface
  (`SatelliteType::debrisArrayShare / debrisRadiatorShare` from its facets' material presets), the class the
  fragment's area, and the area is SNAPPED to the class's mean area (`meanAreaM2`), so its flux scale is 1: it is
  drawn as a mesh close up and Go to works. Large pieces tumble slowly (seconds to minutes a turn: long glints).
- **The small fragments (0.1-1 m) are a density field** (`kessler::DebrisGrid`): each breakup's small population
  is 16 statistical orbits (stratified in size, weight N/16, their own drag deaths) spread over their whole orbit
  into 20 km x 2 deg x 45-deg-heading cells (ECI longitude averaged), fixed-point counts so adding and removing in
  any order is exact. The engine tests every object against it: rate = sum over headings of n x vRel (from the
  heading difference, + 300 m/s) x pi (r + 0.15)^2 x dt, and a hit's projectile size is drawn from the power law
  (most are 10-cm, tens-of-gram pieces: with the cell's mean mass nearly every hit was catastrophic, 1.1M of them
  by day 60). Small-small collisions are not modelled. They replaced 750k representative records of 10-cm
  fragments, invisible (magnitude 13-17) and mostly colliding with each other.
- **Breakups: NASA Standard Breakup Model** (`breakupCollision` / `breakupExplosion`, `BreakupSizes`):
  catastrophic above 40 J/g, N(>Lc) = 0.1 M^0.75 Lc^-1.71 (explosion 6 Lc^-1.6), Lc >= 10 cm, bimodal A/m,
  lognormal dv. A hit that changes nothing visible (no large piece, a surviving projectile, no kill) is not
  recorded. A catastrophic breakup of a formation member damages each cluster neighbour (p 0.6, `damageCluster`:
  the cloud sweeps the ring within minutes, slowly, so they die and tumble). A cratering hit kills a satellite
  with p 1/2.
- **Collisions keep momentum, as two streams, exactly (2026-10-30, issue 20261029_213225; 2026-11-03,
  issue_20261030_180204, kCacheVersion 20; the shipped prebake is still version 18 until it is rebuilt, so the cascade is
  sampled live meanwhile):** a catastrophic pair collision's fragment keeps a share beta = 1 - 0.8 u^1.5 (u hashed per
  piece: 0.2 .. 1, mean 0.68) of its parent's velocity relative to the pair's centre of mass, plus the ejecta kick
  (`planFragments`); then all the records are shifted together so their mass-weighted mean velocity IS the centre of
  mass's (the shares keep momentum only on average: over a breakup's few dozen records drawn at random sides and masses
  the centroid was tens of percent off). The small population does the same (`planSmall`: each sample on its side's
  velocity drawn toward the centre of mass, then the samples shifted to it), and the event's `vel` (the dust cloud's
  orbit, the tracker's Watch, the bands) is the centre of mass's. Until 2026-11-03 both the small population and the
  dust kept the TARGET's velocity: after a head-on hit half the broken-up mass and the glowing dust stayed with the
  target's formation (the report's "debris hangs out with the satellites in formation"). The debris spreads ALONG the
  relative velocity: each parent's pieces run on its own way, the contact zone's slower in the middle. Measured
  (harness `kessler poscheck ev=` -> `momentum`, the report's Starmind pair, 2500 kg each, 15.0 km/s head-on, 66
  records): the centroid relative to the target's own orbit 7502 m/s against 7502 predicted at +10 s (ratio 1.000,
  58 m/s across), 0.997 at +60 s (orbital divergence); the target's pieces 2.5 km/s and the projectile's 14.0 km/s along
  the impactor's travel. The 2026-10-29 first cut moved every piece 0.8 of the way to the centre of mass: one cloud,
  the parents indistinguishable.
- **A pair collision seen up close (2026-10-30, `kesslerFillCollisions` / `kesslerCollisionJobs`, render only):** the
  engine finds a pair together at its step's sample time and spreads the event through the 6-h step, so at the impact
  the projectile's own record is thousands of km away (apply carries the encounter onto the target). For the pair
  collisions within 3000 km, the projectile is drawn on that approach line (`kesslerCollStandIn`: the target's state +
  the relative velocity in its radial / along / normal frame x (t - te)) for the last `kCollApproachS` (45 s): its mesh
  in its own reserved launch chunk (`kCollMeshSlots` 8, below the arm's; not selectable), a sunlit point while
  unresolved (evaluator photometry, kFireBuf). At the impact a flash (kinetic energy of the smaller body x 1e-3 over
  ~0.2 s: a Starmind pair at 2.3 km/s ~-12 mag at 1000 km; capped at flare 400, light curve 0.8 e^(-t/0.08 s) +
  0.2 e^(-t/0.3 s)) and an emissive fireball expanding at 0.15 of the closing speed for 3 s, white cooling to orange
  (e^(-t/0.2 s)). Its resolved peak is capped at ~1.2 of displayed white at its hot size (its size at 30 ms, sHot),
  and its surface brightness then falls as (sHot / s)^2 as the vapour grows. The real
  projectile record is untouched (it vanishes far away at the same second). The candidate list is cached over half an
  hour and rebuilt when the log changes. Harness `kessler state` -> `collisions_near` (in_s, vrel, standin,
  `standin_to_target_m`, target_range_m), `collision_flash`.
  - **Impact timing (2026-11-03, issue_20261030_180204):** the roster's dead / end words are whole seconds, so a target
    hit at te = 2702.14 s was drawn dead (its tumble swung the arrays into the Sun: a white "torn" shape) from 2702.0 s,
    then gone, and so were the ring neighbours the breakup kills: "they turn into debris before the collision".
    `kesslerRosterHold` keeps every roster satellite a collision drawn up close kills (its two bodies, and any satellite
    whose dead or end word is that second within 200 km of the impact) at the words it had until the continuous time
    reaches te, then puts the real words back (never over a word the worker rewrote). Pool pieces are gated by their
    event's time since 2026-10-30 (`kesslerShowGen`). Frame by frame on the report: the target drawn whole at te-0.141
    / -0.050 / -0.010 s (it was dead from te-0.142 s), 4 satellites held, the 11 new records appear at te.
  - **Scrubbing back re-arms both gates (2026-11-04):** a step back in time (`tNow < kShownT`) revisits the generations
    born in the second it lands in (`[lo, hi]` instead of `(lo, hi]`), and `kFirstBounds` is kept (pruned with the
    indexes after 30 days) instead of dropped once its second was entered; `kesslerRosterHold` forgets its scans on a
    step back (`kHoldLastT`). Before, after watching an impact once, a jump from te + 1 ms / 0.2 / 0.5 / 1 s back to
    te - 0.05 s showed the breakup's pieces (a white starburst of its own debris) and no held satellites, and so did a
    1 h 45 min rewind and stepping forward. Measured on the report: `collisions_near` -> `pieces_shown` 0 and 4 roster
    holds at te - 0.05 / - 0.01 s on the first pass, after each of those rewinds and after a 6300-s rewind; 66 pieces at
    te + 1 ms every time; the rewound frames match the first pass (mean abs diff <= 0.28, the -0.05 vs -0.01 s floor 0.50).
  - **Impact frames:** the frame whose sim interval (last fill's time, now] holds te, and the next one, show the flash at
    its peak whatever the time scale (at x1000 a frame is ~16 s: the 3-s flash fell between frames). Harness `kessler
    state` -> `impact_frames` (frames_shown, age_at_cross_s, frame_dt_at_cross_s, flash_at_cross), `impact_frame`,
    `roster_holds`.
  - **The fireball does not engulf a nearby camera (2026-11-03, issue_20261030_180204 follow-up):** the cap used to
    grow with s^2, so the vapour at +0.5 s (sigma ~1.1 km at 15 km/s) was as bright per pixel as the young fireball,
    and its 0.25 / 0.7-s tail kept it at 0.13 of the peak: from the reporter's shot 4 (~1 km off) 98% of the frame was
    tinted brown at +0.5 s. With the hot-size cap and the 0.3-s tail: 7.7% (the mask's floor, moving points), the
    point flash at the crossing 375 -> 361, still flare 360 in the impact frame at 16 s and 60 s frame steps. Mask: luma
    over the te-0.24 s frame by > 8/255.
  - **The impactor is selectable:** the stand-in is listed as a transient (`SelTransient` kind 10, a = the projectile's
    satellite) by `kesslerFillCollisions`; a click on its point or its mesh (`rocketMeshSlotToTransient` maps the
    collision chunk) selects it, and the selection's slot takes its state and own attitude (`kesslerCollTransientElems`).
    The panel reads "<name> - on a collision course, impact in x s (v km/s closing)"; it lapses 2 s after the impact.
    `collisions_near` -> `standin_screen_px` is where to `select at`.
  - **Masses:** the pair's masses are the world's (`kesslerTypeMassKg`: the type's, else `defaultMassKg`; a Starmind is
    2500 kg — the first cut read 500 for types without a mass), and the flash's fireball rides the mass-weighted
    centre of mass along the relative velocity turned into the frame at te (it used the sample time's ECI vector, up to
    6 h of orbit off).
  - **The sound:** one hit per collision on the `impact` layer (sound.md "Collision sounds").
- **The dust up close:** a cloud starts at 0.2 s of spread (was 1 s: ~sv wide at once), and a resolved piece's light is
  its surface brightness (pi x radiance per unit solar irradiance x "Dust cloud brightness"), blended geometrically from
  the point units (toSky, ~10 mag over a surface's physical brightness) as its smallest sigma goes 1.5 -> 8 half-res px:
  from 790 m a young cloud at x30 was a grey-white screen. Distant clouds are unchanged. Setting
  `kessler.collision_closeup` (no UI, default on) switches the stand-in and the flash off (the A/B). QA script:
  `tools/harness/scripts/kessler_collision_closeup.satcmd`. Open: in that view the target ring's own sprites bloom into
  large white ellipses around the impact whether the stand-in is drawn or not (present in the report's shot 1), hiding it.
- **Pool records** (`kessler::PoolRec`, 64-B `GpuKepOrbit` in kesslerBuf from `poolWord0`, satellite indices
  from `activeSatCount` on; appended to the CPU's `satOrbits` with `poolSlot`, so every index-based path works).
  Keplerian with **relative J2**: a child keeps its parent's rates plus the J2 DIFFERENCE between them
  (`recordFromState`), so a cloud spreads around its parent's plane without the roster needing J2 of its own.
  Drag (`decayTime`, Vallado's exponential atmosphere x the storm's heating, `stormDensityGain` 7) sets only the
  death second; orbits do not visibly decay (keeps the GPU exact). From 12 days after the CME (the storm's density
  factor is exactly 0) lifetimes come from a lazily filled table (`World::lifetime`: height x log cdAm, log
  interpolated; a breakup makes ~110 of them: apply time 10.7 -> 3.5 s over 30 days). `sat_rebake.comp` re-bakes
  the pool's phases with the same exact fixed-point turns as the roster.
- **The dust clouds** (round 2, `kesslerFillDust`, `cloud_march.comp` `dustCloudsCS`, binding 23): every breakup of
  the last 3 h with dust (`Event::dustKg` = 5% of the broken-up mass, 30% of it for cratering) is a sunlit cloud
  following its parent's orbit (`Event::pos/vel`), spread by the small fragments' rms kick (`dvSmall`, clamped
  80-500 m/s). **A tube along the orbit (2026-10-20, issue 20261019_231126):** the fragments' positions are the
  Clohessy-Wiltshire solution for isotropic kicks; the cloud is cut into pieces along y (arc length on the parent's
  orbit, the profile's sigma sv sqrt(a^2 + b^2), a = 2 (cos nt - 1) / n, b = 4 sin nt / n - 3t), each piece the
  fragments at that y: radial mean / spread conditional on y, cross-track sv |sin theta| / n with theta = nt + y / r
  the angle THOSE fragments travelled since the breakup (so the tube pinches through the breakup point and its
  antipode, where every fragment's orbit crosses, not at one time for the whole cloud); floors ~30 m + 2% of the
  saturated width, and half the bow of a piece's straight extent. Pieces sit at k D with an along-track sigma 0.8 D
  (the centres' spread deconvolved so the sum keeps the profile), D a CONTINUOUS function of age: min(the spacing
  whose straight +-1.6 D bows < width / 3 from the arc, half the profile sigma), widened when the budget binds
  (every cloud gets 15 pieces, or fewer when it is small on screen: pieces are never closer than 3 half-res pixels at
  its nearest part, so a young or distant cloud is 1-5 Gaussians and the budget holds ~40 clouds, not 25; the rest
  shared out in proportion to what each wants; real-valued, so it never steps).
  The end pieces carry the profile's tails, so pieces appear with ~zero weight; lumps are a smooth noise in the
  fragments' along-track kick E[dvy | y], so they move and stretch with them. Each Gaussian gets a view cone (4
  sigma). It was up to 12 straight Gaussians ~2000 km long (ceil of the length / 1500 km), whose count stepped with
  age (the cloud's width jumped ~50% at 1 -> 2), plus hash lumps keyed by the piece index (reshuffled at each step):
  seen from orbit near the arc, straight "beams" crossing at kinks that stretched and glitched as time ran. Clouds
  are ranked by SURFACE BRIGHTNESS: amp / (along-track sigma x half-width + (nearest part's range x 0.02 rad)^2), a
  hash breaking exact ties (issue 20261020_221525, "Dust doesn't seem to appear anymore": ranked by amp / (nearest
  part's range, floored at 100 km)^2, a late cascade's dozens of IDENTICAL breakups strung past the eye (Starmind on
  Starmind) tied at the budget's cut and the cut's fade drew every one at ~0 — no dust at any gain — and those old,
  wide, faint clouds outranked fresh bright ones). A big parent's cloud is brighter as the square root past
  ~5 t of dust at the default gain (`kAmpKnee`): a 420-t station at x100 blew out the view. The cloud_march cost is
  unchanged (cascade day 1: 4.6 vs 4.7 ms; one station cloud 0.5-1.1 ms). Harness `kessler state` -> `dust`
  {gaussians, light_sum, range_km, sigma_km_geomean}; scripts `harness_runs/stationbk` (`user.satcmd`, `strip.satcmd`:
  the issue's own view, 30-s steps). Never narrower than a half-res pixel. The optical depth through a Gaussian
  is closed form (erf), single-scattered (0.6 HG(0.7) + 0.4 isotropic, albedo 0.25), Earth-shadowed at the ray's
  closest approach, extinguished, hidden by cloud from below like the aurora. **Units:** physical radiance through
  the point sources' scale (`toSky`: a cloud of magnitude m carries the on-screen energy of a star of magnitude m;
  kFluxToFlare x pixel solid angle / (the point style's reference flare x 1.27 px^2 x the sky exposure without its
  EV)). The sky's own radiance is not physical at night (the night exposure is only 5.5x the day's), so a physical
  value in sky units is invisible at any gain. "Dust cloud brightness" (`kessler.dust_gain`, x1 .. x1000, default
  x100): physically a cloud fades below the dark sky within minutes. **No pops** (issue 20261018_211013, day 81 at
  x1000: the budget was full and clouds near the cut snapped in and out as the view moved, and every cloud vanished
  at 3 h still bright): a cloud fades over the last 40% of its 3 h, and clouds near the budget's cut fade by
  priority between the cut and 3x it — never the first 60% of those kept. Harness `kessler state` -> `dust.clouds`
  (each drawn cloud's event, kind, dust kg, age, rank size, amp, cut fade).
- **The dust BANDS (2026-10-31, issue_20261030_175418 "Dust Clouds from kessler"; kesslerFillDust's ring pass):**
  past half an hour a breakup's dust has spread along its parent's orbit far past the point-like cloud the tube
  draws (the along-track sigma is `sv |4 sin(nt)/n - 3t|`: 2 900 km after 30 min for a 500 m/s kick, a whole orbit
  by ~2 h), so its tube pieces are long, thin, ~a millionth of a fresh blob's brightness, its surface-brightness
  rank falls away, and in a late cascade the next breakups — hundreds an hour by day 60 — outrank it: every drawn
  cloud was seconds to minutes old and the debris read as dense glints plus a few round blobs, no dispersed band
  (the report). Each event from 0.5 h to 4 h old is now drawn as a band instead: a chain of clumps (along-track
  sigma `kClump` 400 km, spacing 1.5 sigma, `kClumpsMax` 26) spanning the fragments' along-track 3 sigma, wrapped
  into a full ring once that passes the orbit, each clump with the fragments' radial (marginal) and cross-track
  sigma (20 km floor, `kWidMax` 250 km cap); the event's light (the tube's `Amp` with its age fade) is spread over
  the clumps x `kRingGain` 1.0 in the POINT sources' units — the same display layer as "Dust cloud brightness",
  because a resolved band at a physical surface brightness is ~6 orders of magnitude below the night sky. The
  bands have their own share of the list (`kRingBudget` 144 of `kDustMax` 384; the tube keeps the rest, so the
  cloud march's cost is unchanged: +0.3 ms at the report's view, the CPU kessler block +2.9 ms at day 60, where
  the frame is GPU-bound). The rank is cheap (from the event's own radius, mass and kick; only the drawn bands get
  their orbit propagated) and the bands' rank does not depend on the view, so camera motion never reorders them;
  the one at the budget's cut fades by priority (smoothstep from the cut to 2x it) so time does not pop it. The
  young cloud and its blobs, the fireballs and the point debris (`kFireBuf`) are untouched. At the reporter's view:
  the widest band covers 135 deg of its orbit (37.5 % of an annulus), five bands 544 deg, 115 clumps, 40 km (2
  sigma) thick; the dust's own light lights 63 428 px over 2 display levels vs 8 410 before (+7.5x), the faint band
  0.29 % -> 3.13 % of the frame, bright specks -1.5 %, no new white. One event from 30 000 km: 14.9 deg at 31 min,
  74.3 deg at 2 h. Harness `kessler state` -> `dust.rings` {pieces, arc_deg_max, arc_deg_sum, width_km_max,
  bands[{ev, age_s, arc_deg, width_km, pieces, amp}]}.
  Since 2026-11-03 the bands run from 0.5 h until the event's HOUR is 11-14 h old (they fade by exactly the share the
  rings take: `shellRamp`, by the hour's centre age), each band's light also x exp(-age / 3 d), and a long band's
  clumps are at least 0.8 of their spacing along the track (`kClumpsMax` 36): a wrapped band's 26 clumps sat 1 660 km
  apart with a 400-km sigma, a string of beads.
- **The dust RINGS (2026-11-03, issue_20261030_175418 round 2: "dust clouds disperse as rings around the Earth";
  `kesslerDustRings`).** Every breakup's dust from ~12 h to 30 days old is a full ring around the Earth, merged by
  orbit plane: bins of 25 km in a, 1 deg in i, 4 deg in the node (a late cascade has hundreds of breakups an hour,
  ~150 000 events in the window, 2 224 plane bins at day 61). A bin keeps light-weighted sums (light, normal, a, a^2,
  the fragments' widths, the node-spread terms, mean age) with the weight amp x exp((te - t0) / 3 d), so as sim time
  runs only the HOUR entering (centre age 11 -> 14 h, faded in) and the hour leaving (30 d) are scanned, whole hours
  added and removed exactly; a rollback / clear (`kEventsRev`), a gain change, a time jump or a 5-day-old t0 rebuilds
  it (one scan of the window). Only near-circular orbits with the perigee above 150 km count (a sub-orbital breakup
  was a 3 600-km "ring" inside the Earth and, as the narrowest, took the budget). Widths: the fragments' orbit-averaged
  radial 2.55 sv / n and cross-track sv / (n sqrt 2) sigmas (20-250 km), + the spread of a and of the planes in the bin,
  + the node spread k x age (k = a sin i x the rms difference of the J2 node rates from the kicks' da and di, / sqrt 2:
  the pool's records carry exactly this relative J2, so the ring widens where its pieces do; a 51.6-deg ISS ring 95 ->
  151 -> 302 km at 1 / 3 / 7 days), capped at 1 500 km. Light: the bin's sum x exp(-age / 3 d), knee'd at 2e4
  (above it as the square root: a lone satellite's ring is ~1e4, a cascade bin's raw sum ~6e6 at x1000) x 120
  (`kShellGain`) x `kessler.dust_ring_gain` (no UI, default 1), in the point sources' units. Drawn: the brightest bins
  by surface brightness, 24-64 clumps each (spacing D = 0.8 sqrt(a wR), along-track sigma 0.6 D, so a straight clump's
  bow at 2 sigma stays under half the radial width: with 0.8 D sigmas and D = sqrt(4 a w) the ring's edge was a
  zig-zag, a star around the Earth), `kShellBudget` 256 Gaussians (kDustMax 384 -> 640), the ring at the cut fading by
  priority over 1x -> 1.5x the cut (2x drew 3 rings of the budget in a cascade). The full ranking (~1-2 ms for 8 000
  bins) runs when the sums change or sim time moves 10 min, at most every 0.25 s of wall time; the kept top 64 are
  re-ranked each frame. **Cost** (interleaved A/B in one app, 3 reps, `kessler.dust_rings` false = the bands of
  2026-10-31 alone): the report's view at day 61 (170 km, cascade, x1000) GPU 16.45 vs 16.41 ms (cloud_march +0.18),
  a second set +0.74 (cloud +0.60); the cascade from 12 000 km +0.11 (+0.10); one ring +0.00 (+0.05); CPU frame
  +0.26 / +0.38 / -0.1 ms (the rings' own pass 0.05-0.12 ms; the rest is the bands' longer window); memory: the bin
  table 301 KB at day 61 (1.1 MB before the perigee filter), the dust buffer +20 KB. **Look:** at the report's view
  (x1000) the rings add a haze over 46 % of the frame (+10 display levels, 100 % of directions), from 12 000 km on
  the night side a halo around the Earth over 100 % of the angles (5 % of the frame, +15 levels); point debris
  unchanged (bright specks 754 vs 775), no new white. A LONE satellite's ring (a Starmind breakup, 9.8e3, cross-track
  57 -> 61 -> 79 km at 1 / 3 / 7 d) is drawn but below visibility at x100: the dust's optical depth is its light (the
  Gaussian's w), so a large `dust_ring_gain` makes a thin ring opaque and self-shadowed, not brighter (from 25 000 km:
  44 % of the angles at x10, 29 % at x100, 1 % at x300). Seen from inside Earth's umbra (an observer on the antisolar
  line far out) nothing sunlit is drawn at all, the rings included (an existing renderer limit). Harness `kessler
  state` -> `dust.shells` {bins, drawn, pieces, light, cpu_ms, cache_bins, cache_ramp_hours, cache_hours,
  hours_scanned, events_scanned, rebuilds, ranks, ring_gain, cache_bytes, top_normal_eci, rings[{a_km, incl_deg,
  light, sigma_radial_km, sigma_cross_km, clumps, mean_age_d, events, cut, pri}]}.
- **The engine** (`engineSample`, cube method, Liou 2006): every 6 h of sim time one sample at the step's middle,
  20-km cubes among the roster and the large pieces (sorted in parallel: keys are unique, so the order is the
  same), P = vRel x pi (rA + rB)^2 x dt / V x weights x rate multiplier, x (1 - avoidance) when one is a working
  satellite, active pairs skipped, active satellites subsampled at 0.25 (weight 4); plus every object against the
  grid. **Measured** (round 2, shipped roster, x1): ~150k catastrophic collisions, 314k dead and 155k destroyed
  satellites and ~640k large pieces by day 60 (100k of them L class at day 30) — a runaway, as expected for ~1M
  satellites in narrow shells where each catastrophic Starmind collision makes ~1800 pieces over 10 cm. 30 days in
  14 s (refs 1.2, states + grid 5.4, pairs 2.7, apply 3.5; the worker logs `kessler: engine time`).
- **The worker is lazy** (`KesslerWorker`, below-normal priority, half the cores minus one for position
  sampling): it simulates a minute of the current time warp ahead of sim time (at least 2 days; always the
  storm's first two days at launch) and idles otherwise. The target follows the warp DOWN as well (`wantUntil` is
  set each frame; harness waits are a floor, `kWantFloor`): it only grew until 2026-10-16, so one burst at 1 week/s
  left the worker computing up to 400 days at full tilt after the player was back at 1x (issue 20261016_155342:
  frontier day 342 at sim day 165). `kesslerFillDust` stops its event scan 6 h past now (it scanned to the end of
  the log every frame: millions of future events, 15-25 ms, unbucketed); it is timed in the `kessler` CPU bucket. **Sim time HOLDS at the simulated frontier** (never
  rewinds: a restart pulls the frontier back to the epoch) with a banner, "Computing the debris cascade".
  Rendering is not the cost: at day 60 with a full pool the orbit pass is +0.2 ms GPU and the frame holds vsync
  (harness_runs/kessler2/pw1); what slows a player down is the hold while warping past the frontier.
- **The mirror is BUDGETED (2026-10-20):** `kesslerSync` moves the worker's outbox into a queue
  (`KesslerWorker::syncQ`, main thread only) and `kesslerSyncProcess` works through it in order for `kSyncBudgetMs`
  (3 ms) a frame; `kFrontier` is the frontier of what has been MIRRORED, so under a fast warp sim time holds (the
  banner) instead of a frame freezing (at 1 yr/s a single sync took up to ~200 ms: hundreds of thousands of pool
  changes). `kesslerBreakupInPlace` drains the queue (unbounded) before it rewinds the mirror; `kWorkerDone` waits for
  an empty queue. `satOrbits`, `kPool` and `kPoolGen` are reserved for the whole pool (5M) in `kesslerRestart` (address
  space: pages are touched as it fills): grown a slot at a time, satOrbits (with the 1.4M roster in front) was copied
  again and again, 50+ ms each. The history prune runs at most every 2 s of wall time (`kHistPruneWall`; at 1 yr/s a
  simulated day passes every frame, ~40 ms a walk late in a cascade). GPU pool buffers grow x4 (each growth is a
  ~20-30 ms device-idle stall; ~4 a session).
- **The main thread mirrors** the worker's batches (`kesslerSync`: records, roster changes, events) and uploads
  them (`kesslerRecordUploads`, a persistent 16-MB staging buffer, at most 16k new records a frame, roster words
  in budget-sized pieces — a whole-roster run that did not fit the budget was re-sorted every frame and never
  uploaded: 8.7 ms a frame). GPU pool buffers grow by quadrupling up to the cap (553 MB at 5M records), **without a
  device wait** (2026-10-21, `kesslerEnsureCapacity(ctx, n, inFrame=true)` from the sync): the next size's four buffers
  (visible list + index, kesslerBuf, phase bases) are allocated ahead on a worker once the pool passes 40% of its
  capacity (`kesslerPreallocTick`; ~10 ms of vkAllocateMemory in the frame otherwise), the frame's command buffer
  copies the old kesslerBuf and phase bases into the new ones first thing in `kesslerRecordUploads` (`kGrowCopy`; same
  layout: roster words, deployment era, launch block, records), and the old buffers are destroyed two frames later
  (`kRetired`, `kesslerRetireTick` after the fence). Every record already up stays up and the launch block is kept
  (`kesslerBufGen` is not bumped). It was `vkDeviceWaitIdle` + a synchronous roster upload + every record re-sent:
  20-30 ms, ~4 times a session. Restarts and the init still take the synchronous path. **(2026-10-27) Nothing in a growth waits:** the five sets binding the pool's buffers are swapped for fresh copies (`swapSatRosterDescriptors`, their own pool, retired a few frames later) — the in-place update drained both frames in flight, 24-62 ms at 1 d/s after the CME (the allocation itself is 0.5-2.6 ms) — and the sync stops at the first record past the capacity while the buffers made ahead are not ready (`kesslerGrowReady`: sim time holds at the frontier; `kessler state` -> `grow_holds`). The worker's output is
  mirrored within `kSyncBudgetMs` (3 ms) a frame, all of it: pool changes, then roster changes, events and small-
  fragment streams in pieces of 4096 with a look at the clock after each, resuming where it stopped
  (`SyncBatch::rdNext / evNext / stNext`; the batch's frontier is taken only once it is all in, so sim time holds
  rather than the frame freezing; 6-14 ms at 1 yr/s before). The worker builds each batch outside its mutex and
  hands it over by swap (`KesslerWorker::publish`), and `kesslerSync` only try-locks it (a busy mutex: next frame):
  a replay's publish of ~4.7M pool changes was built under the lock and stalled the main thread ~300 ms. `kEvents` is a `SegVec` (64k-element segments: growing
  never copies; ~10M events late in a cascade reallocated 0.4-0.8 GB at each doubling). A stream invalidates the
  expanded hours it reaches by range (`kSmallReHours.lower_bound`; it scanned every expanded hour per stream). The
  history prune (`kPoolHist`) is a pass over the map's buckets within ~1 ms a frame (`kPruneBucket`; one pass was
  16-105 ms late in the cascade). The
  settings tab reads the counts through `kesslerStateCached`: cheap fields at 4 Hz, the counts from a walk spread over
  frames (`kesslerCountsTick`, 150k records + 150k roster + 600k events a frame, only while the tab reads them; a pass
  counts at the second it began). Until 2026-10-16 it walked 5M pieces, the roster and ~10M events four times a second
  on the main thread: 50-180 ms spikes late in the cascade (issue 20261016_155342). Harness `state` keeps the exact
  walk (`kesslerStateJson`); the piece-lifetime diagnostic is harness-only.
- **Player breakups** (info window Explode -> `infoExplodeSelected` -> `kesslerBreakupFromUi` ->
  **`explodeSatellite(int sat, BreakupKind)`** -> `ExplodeResult{ok, reason}`; harness `kessler breakup|collide`) are
  user events at the current sim time, **at any time** (2026-10-19). One kind in the UI since 2026-10-19: Explode
  (`EV_EXPLOSION`: `breakupExplosion`, always catastrophic, N(>Lc) = 6 S Lc^-1.6, low-dv explosion fragments; the
  SBM's S = 1 is for a few-tonne upper stage, so S = max(1, (m / 2 t)^0.75) since 2026-10-20: the ISS made 239 pieces
  over 10 cm and 7 over 1 m, now ~13,000 and ~330 (96 records); kCacheVersion 13). **A destroyed roster satellite
  is gone everywhere** (2026-10-20, issue 20261019_231126): `rosterFateAt(idx, t, &when)` (0 working, 1 dead, 2
  destroyed, 3 re-entered, from the mirrored roster words) — `selectSatellite` refuses 2-3, `kesslerDropGoneSelection`
  (each frame) drops one that goes while selected, the Missions window's station row reads "Destroyed <date>; no
  more crew flights" with Select / Go to disabled, and crew flights stop (Launches > Crew rotations). The
  ASAT strike (`EV_ASAT`) was the COLLISION model with a 20-kg projectile at 9 km/s (`breakupCollision`: Lc^-1.71,
  collision A/m and dv, catastrophic only while 0.5 m v^2 / M >= 40 J/g, i.e. below ~40 t: a station was only
  cratered) — the same cascade from the player's side, so the button went; harness `kessler breakup kind=asat` keeps it. Not for launch objects (rockets, raising trains,
  retirees). They are computed IN PLACE (`kesslerBreakupInPlace`): the worker stops, the world rewinds to its last
  checkpoint at or before the breakup (`World::rollback`), re-runs that step here with the engine's own events of the
  step (they were sampled before the player acted) and the breakup last, the main thread's mirror is rewound to the
  checkpoint (`kesslerMirrorRewind`: every slot shows the world's generation, the history keeps only older ones, the
  roster's seconds are the world's, streams made since go) and takes the step's records the same frame; the worker
  then carries on. Measured (shipped roster, x1, 1600x900): day 9 30-100 ms on the main thread, day 30 229 ms, and
  no hold; the old full rebuild (`kessler breakup ... rebuild=1`, still the fallback) took 0.2-0.3 s plus a 2.4 s
  (day 9) / 4.3 s (day 30) hold during which every dead satellite and fragment vanished. In-place and rebuilt
  histories are identical (two breakups at day 9.1, counts at day 14: 72698 events, 73890 records, 55638 dead,
  64933 fragments in both). The fallback (no checkpoint: time played back past the kept ones, the journal over its
  cap, a breakup before the world's start) is `kesslerRestart(ctx, kUserEvents, t, t)`.
  - **Rollback journal** (`World::checkpoint/rollback`, Kessler.h): one checkpoint per step start from one step
    before the player's time (`KesslerWorker::simNow`, `keepCheckpoints`), and a journal of what every change
    overwrote (pool records, generations, roster seconds, the free list, the release heap as a multiset, grid and
    death-bucket deposits undone by their exact negatives, retired buckets moved in). Bounded by `journalCap` (1M
    entries, ~200 MB); past it the journal is dropped until the next checkpoint. **Every new mutation of World state
    must be journaled** (`jPool`, `jSimple`, `jDeposit`) or a rollback silently keeps it.
  - **Steps are on a fixed grid** (`World::stepStart`, from the epoch; the worker's chunks end on it) and seeded by
    their index (`stepIndex`), not a running counter: the history no longer depends on when the player's time made
    the worker pause (it did: chunk ends were the player's sim time + the lookahead). A step applies its scripted
    events, then the engine's, then the player's (`sortPending`), in sampling and in replay alike. `kCacheVersion` 12.
  - **Before the CME** (2026-10-19, the user: "if the user clicks the explode button, we presume we want to start the
    Kessler situation"): a breakup starts the world at its own step (`WorldSettings::startT`, `kWorldStart`; a
    disabled scenario is switched on) and that step is computed in place. Before the epoch working satellites dodge
    everything (avoidance 1: they are left out of the engine's objects), so only the debris, the dead and the passive
    roster junk collide; the configured avoidance applies from the CME on. Seconds before the epoch are negative
    (`sat_orbit.comp` no longer gates the roster words on `scenNowSec >= 0`, and takes a floored modulo for the dead
    tumble; re-entries index by floored hours). Going back before the breakup undoes it (pure function of time).
    **The rebuild runs on a thread of its own (2026-10-21):** with no rollback point (any breakup before the world's
    start) `kesslerRestart(..., async = true)` resets the mirror on the main thread and builds the world (buildWorld,
    the hash, the cache read, the breakup's step) on `kRestartThread`; sim time holds meanwhile (`kWorkerDone` false,
    banner "Computing the debris cascade: the breakup"), and `kesslerSync` installs it when ready (`kesslerRestartPoll`;
    every other restart, `kesslerShutdown` and a roster rebuild join it first). Same inputs, same world. The replaced
    world is freed on `kDisposeThread`; the launch state keeps its planes, candidates and rival day index
    (`launchInitBlock(keepPlanes)`: they read only the roster and the catalog; the batches go, freed on a detached
    thread); the worker's idle wait is a condition variable (`KesslerWorker::cv`: its 30-ms sleep made `stopThread`
    take up to ~50 ms). Click frame: 69-79 ms of `kesslerRestart` + the next frame's ~75 ms plane rebuild -> 13 ms;
    the world is installed ~150 ms later. Harness `kessler state` -> `restart_building`. **A breakup clicked while that
    world is building is QUEUED** (2026-10-21c, `kRestartQueue`): its event goes into `kUserEvents` at once (the seed
    keeps its index), and when the world is in (`kesslerSync`, after `kesslerRestartPoll`) each queued breakup is
    applied as if clicked then — in place (`kesslerBreakupInPlace`), else one more async rebuild with every player
    event. It used to join the running build on the main thread (171 ms for the second of two clicks, now 0). Same
    world: `kessler state` counts identical to the joining build (two breakups before the CME, counts at day 9).
    `restart_queued` in `kessler state`.
  - **Time keeps running**: sim time may lead the frontier by ~2 s of the current warp (at most one step) before it
    holds (SatelliteSim.cpp; the banner only while actually held): events published a moment late appear then.
- **Cache and the shipped prebake** (2026-10-28): the engine's events + frontier in `<user data>/kessler/suns_fury_<hash>.pak`
  (the hash covers the roster, types, settings, the kit and `kCacheVersion`), written longer only, never while the player
  has breakups; **the default history is SHIPPED**: `data/kessler/suns_fury_<hash>.pak` -> `<exe dir>/kessler_prebake/`
  (CMakeLists POST_BUILD + sync, `PackageRelease.cmake`; its own folder so the user cache's "keep 3 newest" prune never
  deletes it), 1102 days past the CME (to 2040-07-21), 31.3M engine events, 45 MB. `kesslerRestart` takes the longest
  of user `.pak`, shipped `.pak`, old user `.bin` whose header hash matches (no player events; with them only up to the
  first one's step; a pre-CME breakup: none); the worker opens it on its own thread (25 ms). **Rebuild it with
  `python tools/make_kessler_prebake.py [--days 1100]` after anything that moves the hash** (~20 min; the log says
  "no cache suns_fury_<hash>.pak" when it no longer matches).
  - **Format** (`packEngineEvents` / `EngineEventReader`, Kessler.cpp; magic "KSF3"): per 6-h step the grid hits'
    targets (sorted, Rice-coded gaps, ~1.36 B a hit) and the pairs (varint); time, seed and sampleDt are rebuilt by
    `engineEventOf` (the sampling loop's own function), a grid hit's speed and mass at replay by `World::fillGridHits`
    (the same grid draw, from the world after the step's scripted events and retirement — the replay loop now runs a
    step as `simulate` does: seeds, `retireParticles(ts)`, engine events, the player's) with a per-step check of their
    bits (a mismatch logs and samples live from that step). A step that does not rebuild exactly is stored whole.
    98% of the events are grid hits; ~24.5k events a day from day ~150 on.
  - **Replay is applies only, made parallel** (`World::applyBatch`, every engine step's events, sampling too): each
    breakup's pure outputs (fragments, records, small-population plan: `ApplyPrep`, `prepInputs` / `prepOutputs`) are
    prepared ahead on the engine threads and taken at the event's turn only if nothing it reads was touched since
    (`touchPool` / `touchRoster` in `jPool` / `jSimple`; the record budget re-preps the rest when the pool's fill
    moves it), else made again; the small samples' grid / bucket deposits are queued and summed in parallel
    (`flushDeposits`: integer sums, order-free). `parallelChunks` uses a persistent pool (`ChunkPool`) and every core
    but one while replaying or while the player waits (`setEngineThreadsWide`, `KesslerWorker::userWaiting`). Env A/Bs:
    `SATLIGHTSIM_KESSLER_PARALLEL=0` / `_PREP=0` / `_DEFER=0`. The mirror takes 12 ms a frame instead of 3 while time is
    held (`kSyncBudgetHeldMs`), and its history prune also runs when the frontier moves with time standing still.
  - **Identical to live** (harness `kessler fingerprint` at paused dates 2 / 30 / 100 / 163 / 321 days: replay of the
    prebake == live sampling, and the counts at 163 / 321 / 535 days == the pre-change exe's). Measured (RTX 3070 Ti
    box, 16 threads, harness, default settings, a fresh launch): the supernova Watch (day 163) 416 s -> 41 s;
    2038-06-01 652 -> 62 s; 2039-01-01 901 -> 93 s; 1 week/s from the CME: 95 days reached in 3 min (held all the way)
    -> sim time never held to day ~1100 (frame mean 35 -> 27 ms). The Sun's Fury bookmark (day 0.15) was already
    instant. Live sampling past the prebake: ~1.1 s a day.
- **Memory (2026-10-28, the memory audit; harness `kessler mem` = bytes by structure + working set / private bytes).**
  The 10-35 GB working sets of a long replay were the ROLLBACK JOURNAL: its cap (1M entries, ~200 B each) was checked only
  at the next checkpoint, and when the player's time jumped ahead of the frontier the last checkpoint stayed behind, so
  every change of the replay was journaled until the world got there (a jump to the supernova: 10.7 GB, freed at day 170).
  Now `World::capJournal()` holds the cap every step and `dropCheckpointsBefore` drops a lone checkpoint more than a day
  behind the player (rewinding there would replay it all on the main thread anyway: a breakup then rebuilds async).
  Also: **the event log is ONE copy**: `World::events` is a `kessler::EventLog` (65536-event segments behind a FIXED table
  of 16384 segment pointers: growing never moves an element or the table) and the main thread's `kEvents` is a VIEW of it
  (`KEventView`: the world's log + the count the mirror has taken in; set at install / reset / rewind; elements are never
  written after push_back; shrinking only with the worker stopped). Publishes carry the event count, not copies.
  **Generations dropped at publish** (`KesslerWorker::pruneOldGenerations`): a generation whose slot's NEXT generation in
  the same publish was born more than 8 days before the player's time (the mirror's 7-day history + a day) never reaches
  the mirror, nor do its deaths; `kHistPrunedUntil` takes the replacing generation's birth (going back before it replays,
  as the mirror's own prune). `SATLIGHTSIM_KESSLER_PUBLISH_PRUNE=0` = the A/B. **Backpressure**: the worker waits while the
  mirror has > 3M published pool changes still to take (`kBacklogCap`, `syncBacklog`). `w.poolChanges` is shrunk after a
  big publish (a replay's 30-day chunk left ~600 MB of capacity for good). The mirror drops small-fragment streams, their
  expanded hours and re-entry index hours more than 30 days before the shown second (`kesslerPruneOldIndexes`; raises
  `kHistPrunedUntil`). Results identical: `kessler fingerprint` at days 30 / 60 / 100 / 163 / 365 / 700 / 1100 equal to
  the pre-change exe's, and with / without the publish prune across an in-place breakup, a 5-day rewind and a 40-day
  replay. Measured (harness, RTX 3070 Ti box, 16 threads, default settings; `harness_runs`-style scripts in the audit's
  report): the supernova Watch from sim start 79 s -> 32 s, peak working set 17.6 -> 4.1 GB, settled 5.0 -> 3.9 GB;
  replaying 0 -> 1100 days in steps 720 s -> 260 s, peak 22.7 -> 6.4 GB, at day 1100 9.8 -> 6.4 GB working set. What is
  left at day 1100 (6.4 GB): the event log 2.4 GB (31M x 80 B), the small-fragment streams 0.73 GB (mostly future
  windows), the pool three times over (world 0.53, mirror `kPool` 0.50, `satOrbits`' pool tail 0.48), death buckets 0.22,
  the roster ~0.3, the rest of the app ~1.3. Boot (no cascade yet): ~1.6 GB working set; private bytes ~4.4 GB, ~1 GB of it
  the pool's reserved (committed, untouched) `kPool` / `satOrbits` / `kPoolGen` capacity. The bench line also gives the
  replay split (`chunks` = seeds, retire, log read, grid hits, batches; publish; waiting for the mirror; dropped).
- **Memory, round 2 (2026-10-29, branch memcompact; workstream A).**
  - **The event log keeps 20-byte CORES** (`kessler::EventLog`, Kessler.h): an engine collision's time, seed and sampleDt are
    rebuilt from its step index and pair (`engineEventOf`), a grid hit's projectile mass from (step, target)
    (`gridProjectileMass`), its speed sits in the core in b's place. Everything else — scripted and player events, a
    CATASTROPHIC PAIR collision (the tracker's significant breakups), anything that does not rebuild bit for bit — is
    SPILLED whole; `push_back` checks the rebuild with a memcmp of the whole Event. The 40 bytes of DETAIL (nReal, dustKg,
    dvSmall, pos / vel, a pair's vRel) are per-segment arrays the worker drops (`dropDetails`, at publish) for whole
    segments older than 31 days before the player's time and before every rollback checkpoint; the main thread frees them
    (`reclaim`, start of `kesslerSync`); `prunedUntil` (= their newest + 4 h) makes going back there a replay, as the
    mirror's own 30-day index window (`kesslerPruneOldIndexes`) already does (9 days, the first cut, forced replays the
    old build did not make). `operator[]` returns an Event BY VALUE (`KEventView` too): never keep a
    pointer to one; the counts walk reads `core(i)` / `tUpper` / `tOf` (no rebuild). Detail-less events read those fields
    as 0. A replay of the old world's events (going back, a rebuild) goes through `packEngineEvents(..., tBefore)` (now
    streamed) and the reader, like the prebake, not a list of whole events.
  - **`satOrbits` is a `SatOrbitTable`** (SatelliteSim.h): the roster and the launch block in a vector, the pool's entries
    MADE ON READ from `kPool[j]` (`SatOrbitTable::poolOrbit`, kesslerSetPoolOrbit's old rule; `bindPool` in kesslerRestart).
    A pool entry read through operator[] lives in a per-thread ring of 512: read what you need, never keep the reference;
    writes reach the stored part only. `resize` / `reserve` past the pool's base are clamped; `clear()` unbinds.
  - Measured (harness, uncontended, default settings): day 1100 working set 6369 -> 4128 MB (events 2395 -> 647 MB,
    satOrbits tail 477 -> 0), day 170 3858 -> 3026 MB; fingerprints at 30 / 100 / 163 / 170 / 365 / 535 / 700 / 1100 and
    across an in-place breakup and a rewind equal to the pre-change exe's. Jump times unchanged by these (the supernova
    Watch from a fresh launch 23.6 s, the worker's replay). Left at day 1100: streams 731 MB, the pool twice (world 527,
    `kPool` 496), death buckets 224.
- **World snapshots (2026-10-29, branch memcompact; SatelliteSimKessler.cpp "Snapshots").** Default history only (no
  player events, the world starting at the CME; setting `kessler.snapshots`, default on, no UI). In the user cache
  (`<user data>/kessler/`): the STREAM ARCHIVE `suns_fury_<hash>.streams` (every small-fragment stream in the order made,
  appended at publish: the world keeps none, a restored mirror needs them) and up to 4 SNAPSHOTS
  `suns_fury_<hash>_dNNNNN.snap` (`World::saveState`: pool, generations, free list, release heap, roster seconds, grid,
  death buckets, the compact event log; ~0.9-1.4 GB each). One is written per catch-up while the player waits (time held
  at the frontier: `kesslerSync` sets `snapWant` = the last 30-day mark 8 days before the player's time; never within
  60 days of another): built in memory by the worker (~0.6 s pause), written by a thread of its own. A jump more than 30
  days past the mirrored frontier, and a replay after going back, restart the world ASYNC from the latest snapshot at or
  before the player's time (`kSnapRestore`, the restart job; `kesslerRestartInstall` rebuilds the mirror from the world:
  every slot's current record, roster words, re-entry index, the archive's streams; `kHistPrunedUntil` = its frontier), and
  the worker replays from there. A player breakup turns them off for the session (`snapOn`). Not shipped: a snapshot is
  ~1 GB (the 5M-slot pool alone 0.5 GB), against the prebake's 45 MB. Harness `kessler mem` -> `snapshots`.
- **Roster rebuilds and determinism** (also needed by the cache): `buildOrbits()` draws from `SatRng` (MSVC's
  rand() LCG, one stream per constellation, recorded on the first build — the shipped layout is bit-identical on
  Windows and now the same everywhere), the cluster jitter is keyed by load order (`defIndex`), terminator planes
  by the first build's Sun. `rebuildRoster()` (harness `roster list|remove|add`) restarts the scenario.
- Fragments belong to the "Collision debris" pseudo-constellation (appended at load, no roster satellites, the
  debris type): Show / Hlt work as for any constellation; **highlighted debris is tinted orange and its census
  dot is 8x the satellites'** (`|angularSize| + 100` from sat_orbit.comp to sat_flare.comp: the census dot,
  photometry.highlight_flare 0.01, sits near the eye's limit and barely showed). The separate "Highlight the
  debris" toggle is gone. Labels: "<constellation> #<n> (dead)", "Debris <event>.<k>".
- `GpuSatTypeHeader` grew to 48 B (poolBase, poolCount, scenNowSec, poolWord0); `sat_orbit.comp` binds 12 storage
  buffers (binding 11 = kesslerBuf; the device check asks for 12). CPU bucket `kessler` (harness `perf`).
- Harness: `kessler state | wait [days=] [done] | breakup <sat|sel> [kind=explosion|asat] | collide <a> <b> |
  select [k=] | select dead [k=] [formation=1]`; `state` carries `kessler` (counts now, `pieces_by_class`,
  `dust_clouds_drawn`). Scripts in harness_runs/kessler, harness_runs/kessler2 (round 2: `r2_asat`, `r2_dust_orbit`,
  `r2_long`, `r2_sky`, `r2_check`, `r2_pieces`).
- **Re-entry fireballs** (round 3, `kesslerFillFireballs` + `kessler_fireballs.comp`, SatelliteSimKessler.cpp): every
  re-entry (a death AT the drag decay second: `PoolRec::decaySec`, `World::rosterDecay`; a destruction is not one)
  is indexed by hour (`kReentries`, (satellite, second)) and drawn for 200 s from that second, under where its orbit
  puts it then (the orbit itself never decays: the object vanishes from its altitude and the fireball appears below).
  From re-entry observations (Starlink re-entries over North America, ATV-1, Hayabusa): glow from 100 km,
  brightening; main breakup at 78 km (~81 s) with a flash; 3-80 fragments by mass (`fbFrag`): the remnant keeps its
  speed longest, others slow on 15-115 s timescales (light ones fall behind and below), a few lead the main body
  (+150-350 m/s), cross-track spread ~110 m/s, glow ~ v^2 x the air's density, out below ~2 km/s or 45 km, tumbling
  flicker, a quarter flare once; colour from yellow-white (fast) to orange and red-orange (slowing). Brightness: the
  main body at breakup -4 - 2.5 log10(mass / 100 kg) at 100 km (-7.5 for a 2.5-t Starmind), fragments together about
  as bright as the body; + distance, the extinction along the eye-to-fireball segment (`atmExtinctionMag`) and
  daylight. (A 1 / sin(el) airmass clamped at 0.03 added ~8 mag to every fireball seen from ABOVE: issue
  20261016_154921.) The trails' Gaussians carry that extinction already; in cloud_march.comp they are not extinguished
  again, scattered dust is extinguished to its own (brightness-weighted) distance, not to infinity (looking down, that
  path ran into the ground), and clouds hide both only from an eye below 20 km (the aurora's 95-km test hid trails
  behind the clouds under them from 60-95 km). A fragment's trail is centred 0.55 L behind its head with sigma
  0.28 L (L its last 1.5 s of path), the train's segments sigma 0.3 L: a Gaussian about the path's midpoint with
  sigma 0.45 L reached ~0.4 L AHEAD of the head (issue 20261018_205438). **Material colours** ("Fireball material
  colours" `kessler.fireball_colors`, Off / Subtle 0.4 / Vivid 0.8, default 0.8; `fbFrag`'s matK): each fragment
  hashes a material that tints its temperature colour — steel / soot / composites 38% (no tint), aluminium 25%
  white (AlO bands), copper 12% green (wiring, motors), titanium / magnesium 8% blue-white, sodium 10% yellow-orange,
  lithium cells 7% crimson, strongest while they flare; a big object's remnant (k 0) keeps the hull's temperature
  colour, a small re-entry's (N <= 3) takes one too. Heads are point sprites appended to the finished
  visible list after sat_flare.comp (`kessler_fireballs.comp`, host-visible `kFireBuf` of up to 8192, tagged as city
  lights: meshPx -1, slot index 0xFFFFFFFF; bloom and glare like any satellite); each bright fragment's trail (its
  last 1.5 s of path) and the main body's train (6 segments over ~18 s, fading over 8 s, spreading) are EMISSIVE
  Gaussians in the dust list (`col.w` = 1: no phase, no shadow; the dust keeps at most 3/4 of the 512). "Watch the
  next re-entry" (settings, harness `kessler reentry [watch=1]`, `kesslerGoToNextReentry`): dead satellites over a dark sky
  (Sun below -8 deg there) within 48 h, else the heaviest piece within 6 h; the settings button (watch) at decay + 45 s
  (mid-glow), the side of the track under the clearer sky, FOV 35, the fireball (kind 5) tracked; the harness default
  25 s before breakup, 1x, from 120 km
  beside the track and 80 km back. State: `reentries_now`, `fireball_heads`, `fireball_trails`,
  `fireballs_az_el_mag_km`, `reentries_next_24h`, `piece_lifetimes`.
  **Ballistic pieces re-enter on their own arc** (2026-11-04, issue_20261104_015233 "Odd mirrored debris?",
  kCacheVersion 21): a record whose perigee is at or below the decay floor (120 km) dies, and its fireball starts,
  where its Kepler arc next crosses 100 km descending (`World::deathTime` -> `ballisticEntryDelay`: closed form,
  matches a numerical integration to 0.1 s), not at the breakup second (`decayTime` returns 0 for such orbits). A
  head-on pair collision leaves dozens of these (the pieces keep only a share of their parent's velocity relative to a
  near-zero centre of mass: perigees thousands of km inside the Earth). Until then all of them died at the breakup
  and their fireballs were drawn at 100 km straight under it, each along its own heading: the two sides burst out of
  one point in opposite directions (measured, ev 55997 at 980 km, day 7.9: 58 re-entries at the breakup second, heads
  6-8 km apart; after, 6 unrelated ones). From 980 km a piece with 0.5 / 2 / 4 / 6.9 km/s of horizontal speed now enters
  8.0 / 8.3 / 9.5 / 25 min later, 230 / 940 / 2160 / 9900 km downrange, and stays visible on its way down.
- **Small-fragment re-entries** (round 4, 2026-10-16). Each breakup's small statistical orbits that re-enter inside
  the horizon become at most 4 `kessler::SmallStream`s (merged by decay time; 24 bytes: decay and breakup seconds, a
  window of 15% of the lifetime + the merged spread, 1 h .. 5 days, plane, J2 RAAN rate, count, log mass), published
  by the worker like events (`World::smallStreams` -> `outStreams` -> `kSmallStreams`, by the day their window opens;
  a replay makes them again). The main thread expands them per HOUR (`kesslerSmallReHour`: each overlapping stream
  puts count x overlap / width re-entries in the hour, the fraction rounded by a hash; each at a hashed second, point
  of the orbit and RAAN spread growing over 30 days; sorted by second) — a pure function of the hour, so time runs both
  ways — and `kesslerFillFireballs` draws those within 200 s through the same fragment model as the large ones
  (`drawReentry`: 1-3 pieces, m0 = -1.5 - 2.5 log10(mass kg) at 100 km, minus 2.5 log10 of "Small re-entry brightness"
  `kessler.small_reentry_gain`, Off / x1 / x3 / x10 / x30 / x100, default x30; x1 is physical, meteor-faint: a 0.1-kg
  piece ~+1 from 100 km, matching a kinetic-energy x luminous-efficiency estimate). Not past 1 h/s of warp. Measured
  (shipped roster, x1 cascade, day 100): ~80-90k small re-entries an hour, ~4400 glowing at once worldwide (vs ~50 large
  re-entries an hour); median fragment 0.07 kg, p90 0.27-0.43 kg; 6.4M streams (~155 MB on the main thread; 9.8M /
  350 MB before the merge and packing); +0.4 ms CPU a frame. From the ground they are obvious streaks; from orbit
  moving magnitude 0-3 dots (they are 400-2000 km away). State: `small_reentries` {now, this_hour, streams, gain,
  mass_p50_kg, mass_p90_kg}.
- **Garbage collection: slots recycle** (round 3, `World::addRecord`). A record's slot is released 300 s after its
  death (re-entry or destruction: the fireball finishes) and reused by the next record born after that (LIFO free
  list fed in time order: deterministic, so a replay reassigns every slot identically and the event log's references
  stay valid; a generation counter per slot). `poolCapacity` (5,000,000) therefore counts LIVE pieces. The worker's
  change log carries a copy of every record as placed (`World::PoolChange`; sending only a slot's latest record lost
  pieces that lived and were replaced between two publishes, and their fireballs). The main thread shows, per slot,
  the generation alive at the current second (`kPool` = shown, `kPoolHist` = the generations of reused slots from
  the last 7 days, `kGenBounds` = the seconds slots change hands, re-checked when sim time crosses them, either way);
  going back past what was pruned restarts the world keeping its events (`kReplayPending`: a replay, no sampling).
  Checked: day 60 -> 58 -> 60 identical counts, and a jump back to day 20 (a replay) identical to the first pass.
  `kessler select k=` scans the pool for the breakup's records (they are no longer contiguous); labels are
  "Debris <slot> (breakup <event>)".
- **Engine speed and memory at scale** (round 3, benchmark `harness_runs/kessler2/bench_year.satcmd`, the worker logs
  `kessler: bench day N` every 10 days). The small fragments leave the grid through **death buckets**
  (`DeathBucket`, one dense delta grid per 30-day window of deaths, subtracted when it ends: exact, bounded): one
  stored record per statistical orbit reached 66M records and ~5 GB by day 150. Deposits use 32 samples by a
  rotation recurrence, equal-area latitude bands and octant heading bins (no trig per sample), 4-16 statistical
  orbits by fragment count; `World::stateOf` has a direct Kepler path for pool records (no attitude terms, Newton
  stops when converged); engine events are spread through their 6-h step (`hash01`) and applied in time order (all
  at the midpoint, every collision of a step happened in the same second: dust clouds and re-entries in bursts). **A pair collision is carried to its spread time** (2026-10-24, `Event::sampleDt`, the old unused `firstRec`; kCacheVersion 17): the pair is together only at the sample time, and the projectile's fragments were built where IT was at the event's time, 350-13000 km from the target's (and from `Event::pos`, the dust cloud and the tracker's Watch); now its velocity relative to the target at the sample time, in the target's local frame (radial, along, normal), is turned into that frame at the event time and the projectile is placed on the target: same impact speed, one place (every piece at 0 km from `Event::pos` at the event; harness `kessler poscheck [n=] [ev=]`).
  **Measured** (RTX 3070 Ti, shipped roster, x1, `harness_runs/kessler2/b4`): one simulated year in 507 s (before
  these: 670 s and 13.3 GB); days 0-50 in 28 s, the busiest stretch (days 60-100: millions of collisions) ~3 s per
  sim-day, days 100-365 ~1.2 s per sim-day; working set 1.9 GB at day 50, 3.2 GB at 100, 4.3 GB at 351 (still growing
  ~0.2 GB per 50 days: the event log, 10.9M events by day 368). The 5M slots are full from ~day 80; live pieces then
  fall as the cascade grinds everything down (3.6M at day 100, 1.4M at day 365); by day 100 934k satellites are
  destroyed and by day 365 1.07M. Frame at 1 h/s: 19.5 ms at day 100 (GPU 13.5; the orbit pass 2.2 ms with 5M slots;
  the rest is the engine's threads competing while it computes the busiest stretch), 17.2 ms at day 365.
- Not modelled: orbits shrinking under drag (only the re-entry time), debris-driven ambience, the impact flash,
  small-small collisions, the dust past 3 h (a ring around the orbit: physically invisible), re-entry smoke trails
  lit by the Sun after dusk.
