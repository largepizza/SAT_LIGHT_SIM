# Launches

Trains, retirements, rockets, plumes, flames, pads, droneships, crew and cargo, berthing, the deployment era, mission milestones, mission-ops suite.

Part of the subsystem notes (`docs/subsystems/`, map in `AGENTS.md` §4). Moved verbatim from
`CLAUDE.md` on 2026-11-03. Do not read this file whole: `python tools/outline.py docs/subsystems/launches.md`
lists its sections and topics with line numbers. New notes go next to the section they belong to.

## Subsystem: Launches — trains, retirements, rockets and plumes (2026-10-18, branch kessler)

Plan and progress log: `.plans/LAUNCHES_PLAN.md`. Code: `Launches.h/.cpp` (app-independent: the catalog, the booster
and upper-stage tracks, the orbit-raising phase law, exhaust parcels), `SatelliteSimLaunches.cpp` (planes, batches,
records, visuals, sound, UI, harness), `data/launches.json` (sites, vehicles, programs; copied next to the exe and
packaged). The user's decisions: the roster's size stays constant (launches balanced one for one by re-entries) and
launches follow per-program cadences (no real manifest).

- **A roster satellite is a POSITION; its occupant changes.** Each orbital plane of a constellation a program matches
  (`match`: name substrings) is refreshed once per design life (`life_years` x "Satellite design life"): batch b of
  refresh k arrives at `t_A = (k + phase_p) L + b gap` (phase hashed per plane; a batch = `payload` consecutive slots
  by argument of latitude, a whole number of formation clusters). The rocket lifts off at the plane's next pass over
  one of its vehicle's sites after `t_A - raise - (SECO + deploy)`, the train is deployed low (`park_km`), climbs and
  arrives on its slots exactly at `t_A`, and the satellites it replaces deorbit from there (linear to 200 km over
  `deorbit_days`) and re-enter (Kessler's fireballs, `fbDrawReentry`). The roster record itself never changes. Planes
  are grouped from the roster by (RAAN 1 deg, inclination 0.25 deg, altitude 5 km): a "plane" can hold members up to a
  degree of node apart (Starmind's per-cluster jitter).
- **Everything is a pure function of sim time**: no history. A batch's state is built from hashes and closed forms when
  first needed and cached (`LaunchState::batches`); time warp, reverse playback and bookmarks see the same sky.
- **Pad deconfliction** without recursion: a launch counts the higher-priority (by hash) rivals among the other
  launches' UNDECONFLICTED candidates (`LaunchState::cands`) for its pad within +-90 min; that rank moves it to its
  vehicle's next pad that reaches the plane, then to later passes. Starmind's sun-synchronous planes all pass over
  Vandenberg at nearly the same two times a day, so Starship also uses SLC-4E and the Cape's polar corridor (azimuth to
  200). Residual: ~3 same-pad pairs within 90 min a week of ~66 launches. (Two Starships left Starbase 7 minutes apart
  before it; "yield to the rivals' candidates" stacked the yielders on one later day.) **Starbase Louisiana**
  (2026-10-19; SpaceX's Aug 2026 plan: five complexes of two pads south of Pecan Island, Vermilion Parish, first launch
  ~2029) is ten Starship sites `louisiana_a1` .. `e2`, azimuth 95-200 over the Gulf (east, the 53/30-deg descending
  azimuths, sun-synchronous south); it now takes most Starship launches. Any change to the sites or vehicles reshuffles
  the schedule: re-check the default launch bookmark (it pins a sim time).
- **The phasing** (`buildBatch`, `memberState`): a member waits in the parking orbit, then its radius ramps linearly to
  the slot's; the phase lag to its slot is the closed-form integral of the mean motion over that ramp
  (`launch::rampPhase`), and the wait is solved so the climb ends exactly on the slot (an unsolvable remainder `eps` is
  spread over the raise as a slow drift). **Deployment is a sequence** (2026-10-19, issue 20261019_180951): member i
  leaves the dispenser at `LMember::tRel` = tD + i x `deploy_interval_s` (launches.json: Starship 45 s, else 2), AT the
  dispenser (the stage's aft end + 31 m along the nose and 5.3 m up for Starship's bay door, 9 / 1.6 m for a fairing),
  with the dispenser's VELOCITY + a push `sepDir` = `sep_speed_mps` x (0.97 up, 0.22 aft) + +-`sep_spread_mps` per axis
  (Starship 0.7 / 0.025: the train leaves as ONE string along one free-flight arc, ~50 m apart; stacks 0.45 / 0.12).
  **The whole offset from the member's own path flies free** (2026-10-19, issue 20261019_205535 "serious jitter at
  10x"): `LMember::depOff` / `depVel` (the dispenser less the member's path at release, position and rotating-frame
  velocity, up to ~80 km / 65 m/s: the stage's plane differs from the slot's by up to a degree of node) + `sepDir` are
  propagated as Clohessy-Wiltshire motion about the parking circle (`launch::cwState`), eased out over two days (the
  satellite's own thrusters). The first cut held depOff fixed in the rotating frame and drifted in a straight line
  there: a constant push no orbit has (0.07 m/s^2 across), so the Kepler record drifted off the path between rewrites.
  A member is hidden until its release, and while the train leaves (and 10 min after) — and for a train with a phasing
  residual `eps` (its slow drift is no free orbit) — the batch's records are rewritten every 2 s instead of 20. Harness
  `launch check`: arrival 7 m (= 1 ms of flight), deployment 31 m (the dispenser's offset from the track point),
  retiree start 0, `release_offset_m_max` / `release_rel_vel_mps_max`; `launch deploy [filter=Starmind]` frames the next
  sunlit deployment (of that constellation / program / vehicle); `launch jitter` / `launch legacy on|off` measure the
  record jumps (below); `tools/harness/scripts/starmind_deploy.satcmd` (release to unfolding, paused and live at 10x)
  and `deploy_jitter.satcmd` (per-frame screen motion of a member at 10x, old vs new records; parity while unfolding).
  **Free flight about ONE reference orbit** (2026-10-21, mission-ops suite: Falcon 9 Starlink Gen1 / OneWeb pairs closed
  to 0.8-1.6 m in the first 10 min and again ~an orbit later): each member's drift was linearised about ITS OWN path
  (planes up to a degree apart: their CW errors differed by metres), and a random +-0.04-0.12 m/s per axis did the rest.
  Now (`memberState`, `LMember::ff / ffP / ffV`) a released member flies for `kFreeFlightS` (6 h) EXACTLY as CW motion
  about the stage's parking circle (curvilinear: x radial, y arc length, z across, from its slot / dispenser point at
  tRel; before its release it rides in its slot), then eases into its own path by two days after release as before. Its
  velocity relative to the circle: the train's COMMON in-plane push (`releaseInPlane`: Starship's door and a flat stack
  straight up, a dispenser point 0.97 up / 0.22 back, a column forward off the nose 0.25 up / 0.97 along) + across the
  plane its own (a column's lateral exit + `sep_spread_mps` golden-ratio stepped: cross-track motion is decoupled, it
  never brings two together) + an along-track trim -2 n x0 that cancels the secular drift of its slot's radial offset.
  So every member flies one trajectory shifted in time, and two released dt apart stay >= (its least relative speed,
  `releaseMinRate`) x dt apart; the deploy interval is raised to `kReleaseGapM` (30 m) / that rate (Falcon 9 dispenser
  point ~67 s, Gen1 flat stack ~100 s, Qianfan ~86 s, Guowang's column ~75 s; Starship's 45 s stands). A pure
  along-track push collides with itself ~0.41 orbit later (why the flat stack now leaves upward, its upper member of a
  radial pair first). `launch trace` -> deployment `min_member_gap_1_10min_m` / `min_member_gap_after_60s_m` (now over
  the whole free flight): >= 20 m (mission_ops FAIL below). Offline design check: the CW model of every layout gave
  28-31 m minima over 6 h.
  **Never a purely radial push (2026-10-26, issue 20261026_194340):** Pez (Starship's door) and flat stacks left with
  (sp, 0): a radial CW impulse is a CLOSED 2:1 ellipse, so every Starmind came back to the door it left one orbit later,
  and every orbit after. All but columns now leave with sp x (0.97, -0.24) (`releaseInPlane`): ~2.8 km an orbit of drift
  ahead at 0.7 m/s, the member-to-member rate unchanged. `launch trace` -> deployment `min_stage_gap_after_orbit_m` (from
  3/4 of an orbit on; mission_ops FAIL under 500 m): 1.4 km for Starmind.
- **Unfolding after release** (2026-10-19): a model's stowable parts (`SatComponent::stow`, JSON
  `"stow": {"axis", "root", "scale", "phase"}`; model `"deployment": {"unfold_delay_s", "unfold_s"}`) leave the
  dispenser packed — an accordion array or a folded radiator scaled along `axis` toward the plane dot(axis, x) = root
  (rest BODY frame) to `scale` of its extent — and unfold over `unfold_s`, starting `unfold_delay_s` (+-15% per member,
  hashed) after the member's release, each part over its own `phase` window of the progress P. Then the open book
  (below), and on arrival the operating roster satellite. Starmind: radiators [0, 0.35] then trusses + wings [0.3, 1]
  (folded to the bus edge, y = +-1.5), 10 + 10 min; Starlink V2 Mini / DTC / V3: arrays, 15 + 4 min (estimates in the
  models' `sources`). P = clamp((t - unfoldT0) / unfoldDur) (`SatOrbitElems::unfoldT0/Dur`, `satUnfoldAt`); on the GPU
  the launch record's flag bit 26 + its tumble words (rate, value at the orbit epoch) give `gUnfoldP`
  (sat_orbit.comp). **Photometry**: each lobe carries `GpuSatLobe::stowPack` (the area share lost when fully stowed, x
  (1 - scale), + the window of its largest stowable part, `bakeSatLobes`); its area x (1 - k (1 - smoothstep(p0, p1, P)))
  in `lobeIntensity` / `lobeEarthIntensity` (GPU, `lobeStow`) and `evalSatLobesPosed` / `evalSatLobesEarthPosed`
  (CPU, `satLobeStowFactor`, the `unfold` argument from `evalSatPhotometry`). Parity at P 0.19-0.59: dmag <= 0.01.
  **Mesh**: the component buffer is THREE vec4s per component (pivot / parent, stow axis x (1 - scale) / root, window);
  `GpuMeshInstance::earthShC.z` = 1 - P; `sat_mesh.vert` scales positions (and normals by the inverse transpose)
  (`instStowScale`), and `sat_mesh.frag`'s shadows skip a stowed part's occluder (it is not where its box says).
  **Radial stow** (2026-11-04, Reflect Orbital's sails: `"stow": {"mode": "radial", "axis", "scale", "phase"}`): the
  part's extent PERPENDICULAR to `axis`, from the line through the body origin along it, scales instead (the film grows
  out of a bundle at the hub as the booms push its corners out); its area goes as s^2 (`bakeSatLobes`' k = share x
  (1 - scale^2); the lobe's area still eases linearly in the smoothstep, so mid-unfold flux is approximate). Packed for
  the mesh as the stow axis x (2 - scale), a length > 1 (`instStowScale`'s `radial` out; `sat_mesh.vert`).
  Joints stay at rest throughout (bit 25). P = 1 (every roster satellite) changes nothing. A paused BACKWARD time jump
  leaves the sky TAA's frozen history (meshes are composited in the sky pass) showing the later pose for a few dozen
  frames: `wait 60` after one in a script.
- **Payload fairings and stacks** (2026-10-20, issues 20261019_225243 "Guowang birthed out of the launch vehicle,
  the fairing does not open" and 20261019_223922 "can Starmind actually fit in the Starship?"):
  - **Fairing separation**: a vehicle with `fairing_sep_km` (launches.json: 110 km, crewed shrouds 85-95) drops its
    fairing when the upper stage passes it (`LBatch::tFair`, bisected on the upper track). Two halves
    (`rocket_<v>_fairing.json`: ONE half, profile `sweep_deg` [-90, 90], its origin at its centre; the other is it turned
    180 deg about the stage's +Z; the centre's place on the stage = launches.json `fairing_center_m`) leave with the
    stage's velocity + 1.8 m/s outward, tip outward at 9-13 deg/s and fall under gravity and drag (ballistic coefficient
    60 kg/m^2, air moving with the Earth; `ensureFairing`: RK4 1 s, sampled every 2 s, until below 60 km descending;
    `fairingAt` Hermite). They are SelTransients of kind 7 (key (batch << 3) ^ 5/6) with a full attitude
    (`SelTransient::hasAtt` -> `SatOrbitElems::attFix` via `transientAtt`), meshed through the rocket mesh slots, and a
    sunlit point (mag 4.3 at 1000 km). The stage's model then comes from `upperStageType`: `_upper` -> `_upper_bare` (the
    payload adapter; its top = `payload_base_m`) -> for a crewed stage `_upper_empty` after deployment (the bare one
    carries a Shenzhou / Soyuz MS stand-in). Long March 5B's bare "upper" is a 3-m stub of its core.
  - **Stacks** (launches.json program `"stack"`, `launch::Stack`; `stackSlot` places member i of n in the upper stage's
    body frame `upperFrame` = the rocket models' law: +Z the nose, -Y toward nadir): `pez` (Starship: every member from
    the door at `door_m` 31.1 on the leeward +Y side, flat across the ship: its X = ship +Y (out of the door), Z = ship
    -Z; the ship model has a black slot there, `payload_door_slot`, under a steel `payload_door` that is closed in the
    rest pose and slides up as it "stows": `transientLegs` sets the upper stage's progress to 0 (open) from 90 s before
    the first release to 60 s after the last, over 20 s each way — 2026-10-21, issue 20261021_174219: the slot was
    always open and in the bells' "soot" material, which glows white-hot while the engines burn), `flat` (nadir face down, 1-2 a tier side by side along
    `side_axis`, leaving along the stage's +Z), `column` (around a dispenser column, nadir face out, X along the stage,
    tiers alternately rotated, leaving radially). Top tier first. A member leaves FROM ITS SLOT (`depOff` is the slot
    less its path) with `sep_speed_mps` along the exit direction (`sepDir`, its own LVLH axes), the stack's per-program
    `deploy_interval_s` / `sep_spread_mps` overriding the vehicle's. Before its release (fairing vehicles, from tFair) it
    is drawn on the stage by `launchStackJobs` (48 reserved mesh slots, `kStackMeshSlots`, six chunks below the pads';
    not selectable; members stowed: P 0; the dispenser `rocket_dispenser_<name>.json` too, until the stage is gone) from
    `LaunchState::stackDraws`, filled where the upper stage is listed. Pez members are not drawn before release (inside
    the hull).
  - **Attitude at release**: the member's CPU elements carry `attFix` (`LMember::q0` = the stack's attitude at tRel,
    `omega` a hashed 0.2-0.8 deg/s tip-off, half for pez) and slew into the law over 180 s from tRel + 60-120 s
    (`satAttOverride`, SatPhotometry.h; `AttGeometry::fixW / fixR`, slerped in `evalGroupPoses` for the root group; every
    pose site converts it to its world frame). CPU only: the GPU's point keeps the law for those minutes, and the GPU
    parity check skips it.
  - **Fold** (SatModel component `"fold"`: point, axis, angle_deg, phase): after the stow scaling the part turns about a
    hinge line (rest body frame) by angle x (1 - smoothstep(phase, P)); mesh only (`instFoldAngle`, sat_mesh.vert; the
    component buffer is FIVE vec4s per component since), skipped as an occluder while folded; the photometry ignores it.
    (The CPU shadow accel, `SatMeshRenderer::writeShadowAccel`, reads the same stride and skips folded parts.
    `GpuMeshInstance::earthShC.w` = the accel slot + 1 in its integer part + 0.49 x a burning stage's engine glow in
    its fraction: two parallel 2026-10-20 changes had both written w, so the accel lit every big mesh's "soot" parts.)
    Used for: Starmind's bus halves (the -X half folds 180 deg about y at z 0.51, over the radiator packs: stowed
    6.2 x 5.0 x 2.1 m), array packs folded flat (Qianfan, V1.5, Amazon Leo, V3 over the zenith face; Guowang, OneWeb
    against the flanks) and DTC's antenna under the bus.
  - **Fit** (`tools/make_rocket_models.py`: it measures each model stowed, derives `pitch_m` / `offset_m` /
    `member_center` / `column_radius_m`, checks every vehicle the program flies, writes the dispensers; `--check` fails
    on a mismatch with launches.json; the fairing inner radius = outer - 0.15 m, Starship's bay 8.2 m x 22 m): Starmind
    8 a ship (was 40: the 10 m bus did not fit across the bay at all), Starlink (Gen2 + V3) 30 (was 60), DTC 36 (was 50),
    Starlink Gen1 on Falcon 9 (its own program, 31: a plane's 61 in two launches), Guowang 10 on two tiers of five
    (5.2 m fairings: LM-5B, LM-8A, Zhuque-3; LM-8A's fairing lengthened to 8.5 + 5.5 m), Qianfan 18 (2 x 9; its bus
    2.6 x 1.3 m), Reflect Orbital 25 (pez, below). NOT stacked (no fit as modelled): Amazon Leo (45 of a 2 x 2 x 1.2 m
    bus with a 3 m panel), OneWeb (36 on a column ~12 m tall): they still appear at the dispenser point (stowed).
  - **Reflect Orbital flies Starship only, 25 a ship** (2026-11-04, the user: "the Falcon 9 launching is a bit goofy
    and there is a clown car of satellites from one launch"; was 40 a Falcon 9, 170 kg each). The 55 m quad sail
    (2,819 m^2 of film, 38.7 m half-booms) stows as the booms telescoped to 6% (STOW_SCALE in
    tools/make_reflector_model.py, ~2.3 m nested sections; the film's radial stow uses the same scale so its corners
    ride the boom tips): a 5.45 x 5.45 x 0.81 m cross (stowed_points now reads polygons and radial stows), one a layer
    across the 8.2 m bay, pitch 0.87 m -> capacity int(22.04 / 0.87) = 25 = the payload. Mass is not the limit: film
    ~10 g/m^2 (2.5-5 um aluminised polyimide / PEN is 3.5-7, doubled for seams, ripstop and edge cords) ~28 kg, four
    booms at ~0.4 kg/m ~62 kg, deployer ~30 kg, bus (attitude control for a 2,800 m^2 sail, electric propulsion for
    the 360 -> 500 km raise and drag make-up, power) ~180 kg: ~300 kg (`sat_mass_kg`), 25 = 7.5 t + rack against
    Starship's ~100 t (less to SSO): volume-bound like Starmind. **Release: 150 s apart at 1.0 m/s**
    (`stack.deploy_interval_s` / `sep_speed_mps`; the vehicle's 45 s at 0.7 m/s gives ~31 m, under a 78 m sail's
    span). Two members released dt apart fly one CW trajectory dt apart, but its retrograde-drifting loops CROSS
    themselves at dt ~0.72 orbit (~3,960 s at 360 km): at 180 s x 24 = 4,320 s of release span, `launch trace` gave
    members 1 and 23 45 m apart 4.3 h on. So the span must stay under ~3,800 s: 24 x 150 = 3,600 s, and 1.0 m/s
    makes every pair >= 150 m apart over the 6-h free flight (offline CW over all pairs; distance scales with the
    speed, the crossing time does not). Measured (`launch trace key=` of a 2031-03-12 Gulf-platform launch): `min_member_gap_after_60s_m` 149.7, `min_member_gap_at_release_m` 151, `min_stage_gap_after_orbit_m` 1,983, all 25 unfolding (at 180 s x 0.7 m/s: 45 m, pair [1, 23]). **Cadence**: the roster stays
    balanced by construction (each batch retires the slots it fills): 5,000 mirrors / 25 a launch / 5 years = 40
    launches a year at steady state. `batch_gap_days` 27 (= the 5-year life / the 67 batches of each ~1,667-mirror
    ring) spreads each ring's refresh over the whole life: `deploy stats` 2031-2036 ~40 a year (19, 66, 82 in the
    2028-30 build-out), where the old 3-day gap refreshed all three rings in one 2032 burst (145) and flew none
    in 2033-35.
    **Checked again 2026-10-21 (no change made):** the column fit over 3-8 to a tier, with the models as they are and
    with OneWeb's dishes stowed and wings folded tight (1.3 x 1.32 x 1.06 m) or Amazon's antenna-panel overhang folded
    (2.0 x 2.0 x 1.35 m): no layout fits — OneWeb 36 misses Falcon 9's fairing by 0.08 m at best (6 a tier, 6 tiers),
    LVM3's by 0.27 m; Amazon 45 misses every vehicle by 0.6-3 m (the best New Glenn, 7 a tier). The models' stowed
    boxes are ~2x the real satellites' volume (OneWeb ~1 x 1 x 1.3 m, Kuiper sized for 45 in Vulcan's 5.4 m fairing),
    so a fit needs smaller BUSES, which moves their benchmarks (mallama2020b_oneweb, mallama2026_amazon_leo):
    re-calibrate when that is done. (Per-vehicle payloads — Atlas V 27, Ariane 64 ~35, Falcon 9 24 — would need a
    payload per vehicle, which the plane's batch size does not support.)
  - **Cost**: Starmind's launch rate x5 makes ~5300 live batches (1300 before); launch CPU 1.0 -> 2.1 ms a frame at the
    Starship bookmark (2.4 before `L.livePtr` and the 8-frame full freeing pass); 1.24-1.28 ms after the 2026-10-20 CPU
    pass (see "CPU bucket `launches`"). Guowang's model (realism pass:
    radome over the nadir face, two-panel wings 5 cm lower, yokes, fittings, thruster, star trackers) reads 5.85 vs 4.21
    in its ungated benchmark (5.80 before).
  - Harness: `launch fairing [filter=]` (10 s before a sunlit fairing separation), `launch deploy|fairing|deorbit` select
    the stage once listed (`launchWantSelKey`), `min_members=`, `launch select ... nearest=1`, `kind=fairing`; `launch
    state` -> `fairing_halves_drawn`, `stack_meshes`, `stack_stages`, `upper_stages[].fairing_sep_in_s / model /
    payload_layout / deploy_interval_s`. `tools/harness/scripts/fairing_sep.satcmd` (Guowang fairing + column release +
    unfolding, Starmind's door, Starlink and Qianfan flat stacks).
- **Satellite indices**: roster `[0, activeSatCount)`, then `kLaunchCap` (131072) launch records, then the Kessler pool
  from `poolIdx0()`. Launch records are `GpuKepOrbit`s in kesslerBuf from `kLaunchWord0` (sat_orbit.comp `LAUNCH_CAP`:
  the indices before `poolBase`, the words before `poolWord0`), born/death only "live or not". They hold the member's
  OSCULATING elements at the write time (phases at the current orbit epoch, so they are written after the rebake), and
  a batch rewrites them together every 20 s of sim time or when a retiree's re-entry passes (1200 writes a frame,
  round-robin); the GPU carries them on as Kepler orbits meanwhile. **A record must be the orbit the member is really
  on** (`memberRecord`, 2026-10-19): the velocity is memberState's own derivative (central difference, 0.25 s), an SSO
  plane's precession goes in as `raanDot` (removed from the velocity first), and the epoch phase is computed with the
  mean motion AS THE GPU'S FLOAT HOLDS IT (with the double one, the float's rounding x up to 7 days put a member up to
  ~140-270 m off, differently at every rewrite). Harness `launch jitter` emulates rewrites (the CPU copy and the GPU's
  float Kepler) and reports the jump at each: before (`launch legacy on` rebuilds the old records in the app) 7-290 m
  CPU / 80-300 m GPU mean, after 0.003-0.1 m CPU / ~1.3 m GPU (the float angle's floor, ~3 m at 7e6 m); in the app at
  10x a member's screen motion's second difference fell from 2.4 px max (0.37 mean) to 0.011 px.
  `SatOrbit::launchSlot` -> `launchOrbitElems` for the CPU mirror. 8-slot chunks per batch and kind.
  **The CPU never draws from the written record (2026-10-26, issue 20261026_205544: "the 3D foreground sats jump at
  regular intervals" at 1 min/s):** a record is a Kepler orbit and the path is not one (the climb's ramp, the ease from
  free flight into the slot's path: up to ~0.05 m/s^2 of "thrust"), and under time warp the 1200-a-frame budget lets
  ~32k live records age 20-40 s of sim time at 1 min/s, 130-255 s at 5 min/s: harness `launch jitter` -> `live`
  measured them 10-43 m and 260-1700 m off their path (`record_err_m_max`), each snapping back at its rewrite — every
  1/3-2/3 s of wall time at 1 min/s, i.e. a mesh 60 m away jumped ~10 px. `launchOrbitElems` now rebuilds a live
  member's elements at the CURRENT sim time from its exact path (`memberRecord` at t: osculating there, `mesh_err_m_max`
  0.0009 m at every warp), so meshes, Go to, Track and the readouts follow the path; and the slots recordMeshScene drew
  as meshes last frame (`meshLaunchSlots`) get their GPU record rewritten EVERY frame ahead of the round-robin (<= 256),
  so a mesh's own point / bloom / glare stays on it (`meshed_record_err_m_max` 0.0008 m). Far points keep the old
  cadence (a 1-km error at 300 km is ~2 px from a wide view: open). `launch exact off` restores the old behaviour (A/B).
  **Never assume `satOrbits.size() == activeSatCount + kPool.size()`** or that indices past the roster are debris:
  per-constellation scans run over `activeSatCount`; empty launch entries have `constIdx = UINT32_MAX`.
- **Rockets** (`rocketPos`): the booster's whole flight is a 2D track (downrange along the ground azimuth, altitude:
  the ascent's closed form to MECO, then gravity + drag + boostback (secant-solved to land on the pad for
  `booster: return`), entry and landing burns); the upper stage blends from that ground track into the inertial plane
  by SECO and then flies the parking orbit (`thIns`). **Staging** (2026-10-30, issue 20261030_134600): both tracks'
  points are their stages' AFT ends, so the upper stage starts where it sat on the stack (`stageSepEcef`): its aft end
  `LaunchState::sepM` along the stack's axis from the booster's (the stack model's top less the upper model's, read
  from the rocket models when the launch state is made: Starship 72.32 m, on the hot-staging ring), and the stages part
  at 40 m/s reached over 5 s (a smooth push, peak 12 m/s^2); the term fades out with the blend into the inertial plane,
  zero at SECO. Until then the ship appeared inside the booster. `launch trace` -> meco_staging `stack_offset_m`,
  `stage_gap_m` (from where it sat), `sep_axis_clearance_min_m`, `sep_gap_5s_m`, `booster_flip_rate_max_deg_s` and
  `booster_flip_accel_max_deg_s2` (sampled at 1/60 s). Drawn as point sprites in kFireBuf (the flame's magnitude at
  100 km per stage; a coasting stage sunlit), an emissive Gaussian flame and a dim orange glow of the exhaust it lights.
  **Every upper stage deorbits** (2026-10-19, issue 20261019_180502): 30-90 min after deploying it pitches over to
  retrograde (120 s, through the vertical), burns for `burnDur` (dv to a perigee of 20 km, 45 km for a returning ship,
  at ~12% of the stage's thrust, never more: at least 12 s; the flame and its plume parcels) onto an ellipse whose
  apoapsis is the burn's MIDDLE (`deorbitApoT`: the impulsive equivalent of the finite burn), and reaches the entry interface (100 km, `thEI` / `vEI` / `vrEI`) about half an orbit later
  (`tStageGone` = `tStageRE`, ~32 min). In the burn the stage leaves the circle by `deorbitBurnShape` (the double
  integral of a flat thrust with 1-s start-up / shut-down ramps, normalised) x the ellipse's offset at the burn's end:
  its acceleration is the thrust / mass (~dv / D), and it ends exactly on the ellipse at its speed (2026-10-21; the
  smoothstep blend it replaced peaked at ~4x dv / D, Qianfan's 800-km stage ~90 m/s^2). An expendable stage re-enters there as a fireball; a returning one (Starship's
  ship) pitches up to its entry attitude (nose 60 deg above the velocity, over 300 s) after the burn and flies its
  8-min plasma glow (`shipEntry`: from `vEI` and the ellipse's descent rate at 100 km to ~65 km, slowing to 1.2 km/s
  by 45 km, magnitude ~-4 at 100 km near 70 km up; then it lands: see the catch below; harness `launch ship`). Until then only
  expendable stages burned (40 s, while sinking 30 km on their circle) and Starship's ship jumped from its 350-km orbit
  to 100 km at the entry interface without turning or burning. The ship's entry is listed under the upper stage's own
  key, so a selected or followed ship stays selected into its entry. **Its glow is on the belly** (2026-10-21, user: "re-entry emits from the rocket
  engine"): the point, its light on clouds and the wake start at the body's middle (the upper model's bounds centre
  along the nose) standing off the windward side (perpendicular to the axis, toward the velocity) by the body radius
  + 0.6 m, not the track point (the model origin = the aft end, the engine bells); as the mesh resolves (2 -> 8 px) the
  point gives way to the shared **entry-plasma emitter** (2026-10-21 validation, `plasmaGauss` / `plasmaHullLight` in
  launchFillVisuals, also used by the capsules and Dream Chaser): a thin shock layer along the belly (sigma 0.4 x the
  half-length along, 0.75 x the radius across, 0.45 m thick), a short hot wake behind the body (0.12 of the layer's
  brightness) and the 6-s path wake (0.15). Each Gaussian's light is, unresolved, the point's on-screen energy and,
  resolved (its middle sigma 1.5 -> 8 px, geometric blend as the flames), a SURFACE BRIGHTNESS: the layer ~4x a sunlit
  white face x the plasma intensity, held at its face-on value whatever the view, capped at 2.0 of DISPLAYED light (x
  `skyExposure()`: the night exposure needs no separate factor), the wakes keeping their ratio to it; faded as the eye
  comes inside one (3 -> 1.5 sigma). The sheath was in the point's units (`toSky`, ~10 mag over a surface): a white
  ellipse / disc several times the vehicle. The plasma lights its own hull: three PHYSICAL mesh local lights 2 m under
  the belly (`meshLightAdd(..., physical = true)`: not lifted at night, not adapted; irradiance 1.5 x the capped layer
  brightness x its share), so the belly and flaps glow pink instead of a dark silhouette; its cloud light no longer
  adds a mesh light (meshK 0: its rs was 160 m). `launch state` -> `entry_plasma[]` (range, res_k, s_asked,
  s_disp_capped, view_hold, sigmas), `sky_exposure`; harness `launch ship day=1` (the entry over a sunlit ground).
  Booster entry burns stay at the engines (they are burns). A stage may vent what is left, spinning
  (`vent_chance`; after its deorbit burn, or 10 min after deploying for a returning stage): ~10 t over two minutes as
  slowly spreading ice (30 m/s, 2000 m^2/kg), each parcel a stretch of the 700 m/s jet, never decimated: seen from
  below, a dump spiral like the 2009 Norway one (harness `launch vent`). Boosters land on droneships (below). Harness `launch deorbit` frames
  the next sunlit stage as it starts to turn; `launch state` -> `upper_stages` (each flying stage's timeline in seconds
  from now, burning, altitude, the nose's angle from retrograde); `launch check` -> `booster_nose_max_deg_per_s` (18),
  `upper_nose_max_deg_per_s` (2.0) and `upper_max_step_m_per_s` (7893: 1-s steps through the burn and the entry
  interface, i.e. no jump).
  **A returning ship's vent is a CONTROLLED dump (2026-10-26, issue 20261026_194151):** emit mode 4, out of its aft end
  along its own axis (`stageNose`), ~10 t over 3 min at 350 m/s, 20 min before its deorbit burn and at least 3 h after
  deployment (`ventStart`) — it was 10 min after deploying, spinning, among its train. Expendable stages keep the spiral
  after their deorbit burn (the real Falcon 9 dump spirals).
- **Attitude** (`stageNose`, 2026-10-19): the booster's nose is a pitch angle in its trajectory's vertical plane
  (`boosterPitchRaw`: along the velocity on the way up, engines first after staging, along the thrust in its burns,
  upright for landing; branch [-90, 270) deg so nothing wraps), smoothed by `boosterPsi`: a causal Hann window
  (`kBoosterTurnS` 16 s) over the raw pitch sampled on a FIXED 0.5-s grid of tau, so a step (the flip after MECO, a
  burn's start) is a slew whose turn rate is the window's bell (the 180-deg flip peaks at 22.5 deg/s) and whose angular
  acceleration starts and ends at zero, and nothing steps between frames (2026-10-30, issue 20261030_134600: the
  10-s box over samples at tau - 0.5 k moved each sample past the step as tau crossed it, so the flip came in
  9-degree jumps every 0.5 s). The booster turns about its centre of mass (0.3 of its length up), not its aft end:
  `boosterPivotShiftEcef` moves the track point by zc (n_stack - n) from MECO, fading out over the 40 s after the flip.
  The upper stage (`upperNoseRaw`):
  its velocity to orbit and in orbit, the deorbit pitch schedule above, averaged over 6 s. Averaging nose VECTORS (the
  first cut) spun the booster ~340 deg/s after an RTLS boostback, where engines-first and the thrust point opposite
  ways; before that the axis switched instantly at staging and for the deorbit burn.
- **Rocket meshes** (2026-10-19): `tools/make_rocket_models.py` writes `rocket_<vehicle>_{stack,booster,upper}.json`
  (16 vehicles; +Z toward the nose, origin at the aft end of the body = the track point, engine bells below it; one
  group, +Z on the "velocity" target, -Y (the ship's heat shield) toward nadir). Reworked the same day against
  references (issue 20261019_181320; each model's `sources` cite them): bodies, noses (tangent ogives with blunt tips),
  fairings, strap-ons (Soyuz's tapered cones leaning onto the core) and bells are PROFILES, flaps / fins / strakes
  FINS; Starship's ship is a 38.6-m barrel + a 13.5-m ogive with the windward half (arc 176-364 deg) in `tiles`
  (`hex_tiles`), tapered aft flaps (straight trailing edge) and forward flaps shifted leeward (20 / 160 deg) on the
  nose's slope; Super Heavy 72.3 m with the integrated hot-staging ring, three grid fins (Block 3) and 33 Raptors
  3 / 10 / 20; the ship 3 sea-level + 3 RVac. EVERY engine is its own render-only bell (open, sooty inside) named
  `engines_*` / `upper_nozzle_*` / `side_nozzle_*`: engineGeomOf takes the lowest exit and the radius the bells span
  (profiles count, the exit at their bottom), so the flame grows out of the actual engine cluster. **No surface may
  cut a bell** (2026-10-20, issue 20261020_181510): every bell's throat (exit + 0.68 x its length) sits at or below the
  aft cap / heat shield over it. The ship's barrel used to end in a cap at the track point with the bells half above
  it, so their glowing interiors read as crescents: it now has an open render-only aft SKIRT (`aft_skirt`, 3.3 m,
  black inside) down to the track point, the RVac exits 0.15 m inside its bottom, the sea-level bells recessed (exit
  0.8 m up), the `aft_heat_shield` at 3.3 m above every throat; the photometric barrel starts there. Super Heavy's
  and Falcon 9's core exits moved down 0.15 / 0.1 m for the same rule. Grid fins use the
  `gridfin` lattice material (stowed folded up on Falcon 9's stack, deployed on its booster). Photometric: the bodies,
  tanks, fairings, capsules, strap-ons and Starship's flaps; detail (bells, grid fins, legs, bands, chines, raceways,
  escape towers, the hot-staging ring) is render-only. Interstage sleeves stay render-only: a separate interstage
  cylinder on the core's cap was a coincident surface that broke the occlusion samples. Loaded at startup as extra types like the debris kit (`rocketTypes`; the accuracy
  gate only loads and validates them, `cmake/AccuracyGate.cmake`: their selftest took ~2.5 min). Each listed rocket
  transient carries its stage's type and its BODY AXIS (`SelTransient::axis`): the Earth-fixed velocity on the
  track (in ECI the pad's rotation tilts a rising rocket), up on the pad, engines first for a booster after
  staging and an upper stage in its deorbit burn, belly first for the ship's entry (60 deg above the velocity).
  The axis rides in `SatOrbitElems::fixAxis`, which `satOrbitStateAt` returns as the attitude's "velocity".
  Rockets past ~1.4 px go through `recordMeshScene` (`launchMeshJobs`: up to `kRocketMeshSlots` 7 slots of the
  reserved last chunk, bound to their keys) with the evaluator's photometry; the flames come out of the model's engines
  (see **Flames**), a coasting sunlit point gives way to its mesh, and the exhaust's glow Gaussian is a night effect
  (x (1 - 0.95 dayness): up close by day it was a white ellipse hundreds of metres across). A
  selected rocket takes its type in the selection slot, so Info / Go to / Track work (Go to rides the fixed state).
- **Raising trains fly "open book"** (2026-10-19): a train member's record (kind 0, until arrival) carries bit 25 of
  `typeConst` (sat_orbit.comp: `alignTerminator` -1, every joint at rest) and `SatOrbitElems::restJoints` /
  `AttGeometry::jointsRest` on the CPU: the arrays flat with the bus, edge into the flow, the low-drag
  configuration that makes fresh trains bright (SpaceX's description; the operating "shark fin" is the roster
  satellite on station). Models whose rest pose is flat (Starlink V2 Mini / V3 / DTC, Guowang, Qianfan, OneWeb,
  Starmind) show it; V1.5 and Amazon Leo rest as a vertical fin. CPU/GPU parity holds (dmag ~1e-5).
- **International launches** (2026-10-19): Qianfan (Spacesail) is in the roster as one Shells entry (phase 1 as
  flown: 72 x 18 at ~1070 km, 89 deg; two ESTIMATED shells toward the ~14,000 planned, 11,664 in all; model
  `qianfan.json`, estimates) with its own program (Long March 6A / 8A, Zhuque-3, Tianlong-3, Pallas-1). Zhuque-3
  (LandSpace, Jiuquan LC-96B), Tianlong-3 (Space Pioneer) and Pallas-1 (Galactic Energy) fly from the Jiuquan
  commercial pads and the Hainan commercial launch site (pad coordinates approximate); Guowang also flies
  Zhuque-3 / Tianlong-3, Amazon Leo Falcon 9. A plane is refreshed in one burst per design life, so a
  constellation's cadence follows its plane count: Qianfan ~6 launches a month, Guowang ~4, Starmind ~10 a day.
  Harness `launch why [hours=]` counts the cached batches by program and reason.
- **Plumes** (`buildParcels`, `launch::makeParcel`): parcels of exhaust every 1-1.5 s of each burn, integrated once per
  launch (only for launches within ~4500 km of the eye and not past 1 h/s of warp) with entrainment: the gas expands
  at ~2 km/s while it outweighs the air it has taken in, then diffuses (above ~80 km the coupling to the air falls to
  3%: the exhaust largely passes through air whose mean free path is ~1 km, and the bell grows to hundreds of km); momentum mixes with the swept air (exact for
  any step), gravity acts on the exhaust's share, Coriolis and centrifugal in the Earth-fixed frame, a hashed wind by
  altitude that turns and varies with height (tides / gravity waves above ~35 km, `windPhase` per launch: trails twist
  and kink as they age). Low down a narrow trail the wind carries, high up a bell that expands for tens of km and falls
  back into the thicker air. The exhaust condenses over 0.25 s (low) to 1.5 s (high) before it scatters (`cond`).
  **A burn is a polyline** (2026-10-19, issue 20261019_175126; `Parcel::stream`, launchFillVisuals' `addPlume`): its
  parcels are knots, each Gaussian the chord from the previous drawn knot to this one as they are NOW (centre the
  midpoint, along-trail sigma 0.35-0.5 x the chord, condensed share and radius the mean of the two ends), and the
  youngest knot joins the burning stage's nozzle with a HEAD segment that grows with the burn and is exactly the segment
  that replaces it when the next knot is born. Each parcel used to be drawn as the stretch of path it was emitted on: a
  new one popped in every interval as a needle reaching ahead of the stage (the user's "intermittent, squished"
  trail), the upper stage's young vacuum parcels (2.5 s = 8.5 km apart, spreading 2 km/s) read as puffs, and the frame
  pulsed at every knot (the newest segment born dimmer than the head). The budget fades Gaussians near its cut by
  priority (0 at the cut, full at 3x it). **Billows**: cloud_march.comp modulates each plume Gaussian's optical depth by
  the baked Perlin-Worley (`cloudNoiseTex.r`, mean `kPlumeNoiseMean`) at the ray's closest approach in the Gaussian's
  own frame (2026-10-21: the offset from its centre in Earth-fixed axes / its width sqrt(2 / trace M); it was the eye's
  ENU axes x the inverse sigma along the view, which rescaled the billows with the view angle on elongated segments and
  swam as the camera moved), seeded per parcel (`ib.w` = 4 x seed + the unread sky light); only where tau > 0.01 (on every
  faint tail it cost up to 7 ms; now within noise). A raymarched plume (density field + light march per pixel) was
  not built: hundreds of overlapping Gaussians per pixel would need a voxelised field or a per-tile list first. Cross-section `kappa`: 10 m^2/kg low (condensation, soot) to 1000 m^2/kg above ~90 km (the exhaust's water as
  sub-micron ice: an upper estimate, ~2000 if all of it froze; 300 left the bell's wide part invisible); albedo from
  `soot`. Parcels past 2 / 10 / 30 min are drawn every 2nd / 4th / 8th once they are 2x / 4x / 8x wider than their spacing (thinned
  earlier, a trail showed gaps), plumes at most 384 Gaussians a frame (wide ones cover the screen: ~3 ms at 1000). Once
  a high parcel has swept up about its own mass of air it is drawn as a core + its limb-brightened rim: across a
  lone parcel (or a trail seen end-on) a ring of 7 Gaussians round the line of sight, along a trail two Gaussians beside
  each segment, as long as it, so neighbours join into the tube's two edges (45% of the light in the rim).
  (Rings on every parcel, 5 s = 10-35 km apart on the upper stage, drew its insertion burn as a pulsing string of
  circles: the user's "oscillating" second-stage trail.) **Colour:** the Sun's light at the parcel through the air
  (grey-ish k per channel), x the ozone shell's Chappuis absorption along the Sun's path (cloud_v2_sunlut.comp's model:
  vertical 0.019 / 0.027 / 0.0016 at 680 / 550 / 440 nm x the crossing secant, counted once, capped at 12: 30 twice
  turned the plumes between the orange trail and the white bell magenta, the user's "odd purple trail"), x the
  sub-micron ice's spectral scattering (Angstrom 1.5, luminance kept) from 30 to 100 km, only as far as the sunlight
  there still has its blue (B/R of its transmittance):
  why twilight plumes, like noctilucent cloud, are off-white to blue (harness `launch state` -> `plume_high[].sun_col`
  ~(0.73, 1.0, 1.4) above 100 km). They go into the dust list as `col.w = 2`
  (`kMaxDustClouds` 1024: the Kessler dust takes at most `kDustMax`, the fireball trails up to `kFireTrailMax`, plumes
  and flames the rest, brightest per pixel first); cloud_march.comp sums their optical depth, averages their light per
  unit of it (single scattering by an ice phase g 0.8, multiple scattering growing with the optical depth), composites
  `1 - exp(-tau)` in front of or behind the clouds by their mean distance, and dims what is behind by
  `exp(-tau) + 0.85 albedo (1 - exp(-tau))` (the sky's light it scatters back; `col.w = 2 + albedo`): a clean plume in
  the dark barely dims the sky, a sooty trail silhouettes (as pure absorbers they were dark smudges at night).
  Each Gaussian carries a view cone (80-B `GpuDustCloud`): the shader skips it with one dot product. At night the
  plumes' sunlight is raised up to 10x as the eye's sky darkens (the night exposure is not physical: see the dust).
  **The budget keeps every launch's plume (2026-10-26, issue 20261026_193912: an older bell left in a frame as the next
  launch's young trail outranked it):** (1) parcels are decimated by WIDTH (sigma / spacing > 3 / 6 / 12 / 24 -> every
  2nd / 4th / 8th / 16th, from 20 s of age; it was age-gated from 2 min, and a vacuum bell's 1-s parcels were 20
  overlapping 150-km Gaussians at 90 s); (2) each launch drawing plumes is reserved its strongest min(48, half the budget
  / launches) Gaussians (`Cand::grp` / `key`), the rest by priority. The bell now fades out over ~1.5 min.
  **No removal is a step (2026-10-26, issue 20261026_210053 "plume disappearance still instant", a launch's bell seen
  from 53.6 km over the Gulf going out piece by piece between T+62 and T+65 min):** the cause was the parcel LIFETIME —
  a parcel ends at `kPlumeAgeHigh` 3600 s / `kPlumeAgeLow` 2400 s (`Parcel::at` returns false) and its condensed share
  was still exp(-age / tauEvap) = 0.2-0.5 there, so each stretch of trail vanished whole as its parcel aged out (the
  budget was not binding: 310 of 384 drawn, cut 0; `launch state` -> `plume_groups` per launch: candidates, drawn,
  parcels alive, largest thinning, summed cross-section). Now: (1) `makeParcel` multiplies `cond` by
  1 - smoothstep(0.4, 1) of age / maxAge, so a parcel reaches 0 at its end; (2) old exhaust keeps WIDENING:
  `ParcelOpts::richK` adds richK sigma^(1/3) to d(sigma)/dt (Richardson's 4/3 law, same cross-section: thinner as it
  widens), 0.18 x "Plume growth" x `launches.plume_dispersal` (key only, default 1) in the troposphere, half above
  12-20 km, none in the bells above 60-90 km (widened they became a grey veil over the sky and +2.7 ms of composite):
  a low trail ~1 km wide after 10 min, ~5 km at the end (it was ~250 m); ground clouds keep their own growth;
  (3) the knot THINNING is continuous: lam = log2(width / 1.5 spacing), from 20 s of age at most a level per 4 s, up
  to every 64th (was 16), and the knot the next level drops slides onto the midpoint of the two that stay (radius,
  `cond`, sweep too) over the last 60% of lam's step, so its removal changes nothing; ground-cloud puffs (outside a
  burn) were never dropped but drawn x their step (an accidental 8-16x from 20 s of age, which their look was tuned
  with) — that factor is now 2^lam, continuous; (4) the reserved share is eased (`plumeResEased`, 0.25 rank a frame,
  snapped when it must halve: a jump) and its last 8 ranks fade (`Cand::res`), and the CPU skip tests a launch's own
  reserved boundary (`LBatch::plumeResPri`) instead of the first 48 emitted. Measured (RTX 3070 Ti, 1600x900, the
  report's view, crop of the plumes, mean / max frame-to-frame change in `vsheet.py`): 10x 0.123 / 1.29 -> 0.109 / 0.47,
  1 min/s 0.264 / 1.48 -> 0.244 / 0.60 (the max now at the H.264 keyframes); the T+62-min bell is already faint there
  (by design). Cost: twilight bookmark at T+10 min cloud_march 7.9 -> 7.8 (dispersal 0) / 8.0 (0.5) / 8.3-9.2 ms (1);
  beside a pad after it 6.5-6.7 -> 7.3-7.7 (0) / 8.2-8.6 ms (1), the budget then admitting wider, fainter Gaussians;
  CPU within noise. Open: a cost-aware budget (coverage, not count).
- **The second stage's jellyfish and hot staging (2026-10-30, issue 20261030_134020 "Second Stage Jellyfish").** The
  upper stage's vacuum exhaust used to be invisible: every parcel expanded at 2 km/s in every direction (~300 km wide
  3 min after leaving, optical depth ~2e-5), its young Gaussians lost the budget to the wide old bells, and the sim's
  night exposure (not physical) left sunlit ice in a dark sky at ~1e-3 of display white. Now (SatelliteSimLaunches.cpp,
  constants under `kPlumeAgeHigh`): (1) a stage-1 parcel above 60-120 km spreads at `kVacExpandK` (0.3) x the exhaust
  speed, the plume's divergence (`buildParcels`' `vExpE`); above 100-160 km its ice condenses ~3x faster
  (`makeParcel`'s `tauCond` x 0.3: the youngest exhaust is the brightest, the "nucleus"); (2) at night its sunlight is x
  `kVacNightBoost` (5) above 70-130 km (`vacK` in addPlume); (3) above 90-140 km every exhaust Gaussian also gets the
  limb-brightened rim (`freeShell` 0.5: a freely expanding parcel is a shell, not a ball) — not hot-staging lobes or
  streaks (`curKind`); (4) **streaks**: the ascent's vacuum parcels (`isAscentVac`) give `kStreakShare` (0.4; 0.24 for a
  single nozzle) of their cross-section, out to `kStreakAgeS` (240 s) and above 100-150 km (`streakShareAt`), to K
  polylines (K = launches.json `upper_engines`, at most 8; 4 filaments for one nozzle): knots at fixed ages behind the
  stage (0, 4, 12, 28, 55, 100, 160, 240 s; age 0 = the nozzle while burning), each offset from the plume's axis by
  `kStreakOffMin`-`kStreakOffMax` (1.0-1.8) of the exhaust's radius there at a fixed angle round it, so they fan out of
  the stage wider than the core (the interaction sheets between neighbouring nozzles' plumes, riding the gas's edge);
  width `kStreakWidth` (0.1, 0.15 for filaments) x the radius; the radius used never shrinks with age (a running max
  over the knots: the older, lower trail's air-confined parcels drew the streaks back into a spindle). (The first cut,
  0.45-0.8 and 0.06, was too narrow for the user.) (5) **the budget**: above
  60-100 km a Gaussian's priority is x tau / (tau + 1e-3), and the young vacuum plume and the hot-staging lobes
  (`Cand::jelly`) are kept like a reserved share and never skipped early. (6) **Hot staging** (`hot_staging` in
  launches.json: Starship, Soyuz-2, Long March 2F): 18 lobes (`Parcel::turb`, puffs, never decimated or shelled) round
  the interstage at t1 - 2 .. t1 + 0.6 s, sharing the upper stage's exhaust for 2.6 s, thrown sideways and back at
  0.4-1.4 km/s, starting 250-550 m wide and growing by Richardson's law (richK 25) to a few km in half a minute, kappa
  3x the trail's, albedo 0.85, tint (1, 0.78, 0.5); drawn with seeds 192-255, which cloud_march.comp reads as turbulent
  (`turbP`: ~2x the billow contrast and a second octave; ordinary plume seeds are 0-191). Harness `launch state` ->
  `plume_vacuum` (above 100 km and the lobes, youngest first: kind 0 exhaust / 1 lobe / 2 streak, tau, priority against
  `plume_cut`). Measured (RTX 3070 Ti, 1920x1009, the report's view): cloud_march 13.5 -> 9.8-10.7 ms (the dropped faint
  bells), the `turbP` shader change within noise and at 128 registers, launches CPU 2.24 -> 1.87 ms. Open: from the
  ground 600 km away the staging cloud is a few pixels (physically sized).
- **Jellyfish round 3 (2026-10-30, the user: "60% there").** The streaks are sharp only near the ship: their share
  fades from 0.35 x "Jellyfish streak length" (`launches.jelly_streak_length_s`, 30 s) to length x (1 + 1 / diffusion)
  and their width grows x (1 + 3 diffusion age / length) (`jelly_streak_diffusion` 1): downstream they hand back to the
  diffuse core. They follow the trajectory's curve: 13 knots at ages end x (j/12)^2, each knot's frame from its
  neighbours, each segment cut into pieces along a Catmull-Rom curve, no piece longer than ~4 x the streak's width (at
  most ~140 a launch; along-sigma 0.6 x the piece). Nothing reaches ahead of the nozzle: while a stage burns, every
  trail Gaussian of its launch whose 2.5-sigma extent along the flight direction passes the nozzle is shrunk about its
  centre (same cross-section; dropped below 2%). Settings > Launches > ROCKETS AND EXHAUST, slots 428-434, keys
  `launches.jelly_streak_count` (0 = one per engine), `jelly_streak_brightness`, `jelly_streak_spread`,
  `jelly_streak_length_s`, `jelly_streak_diffusion`, `jelly_nucleus_brightness` (x the light of the young core, e-fold
  5 s of age), `jelly_staging_cloud` (x the hot-staging lobes' cross-section); "Reset exhaust to defaults" covers them.
- **What the Gaussian list costs, and the three culls (2026-10-21).** `dustCloudsCS` costs every texel the work of
  every Gaussian whose cone covers it; old, wide or Earth-shadowed exhaust (bells hundreds of km across, trails in the
  night) covers the whole sky while adding nothing: in the droneship bench view 160 of 390 Gaussians were in every
  texel, ~1.5 ms of their lighting alone. (1) **Per-tile cull** (`cullDustForTile`, cloud_march.comp, before main's
  early return like `cullBeamsForTile`): each 16x16-texel workgroup tests every cone once against the tile's cone
  into a 1024-bit LIST-ORDER mask (so the sums keep their order: the same image as the full scan), plus per Gaussian
  the engine lights whose reach can touch it (`sDustLM`: reach + 8 sigma_max, sigma_max^2 <= the covariance's trace
  from the inverse scaled to O(1) — entries reach 1e-12 for a 1000-km bell). (2) **Per-texel faint skip**: a plume
  whose optical depth here is < 1e-5 and whose light (Sun / sky colour x 2.6, + each reaching engine light at that
  point) is < 2e-7 of white at the frame's exposure (DustBuf header word 2 = `skyExposure()`, written by
  `kesslerFillDust`) is skipped before its lighting; summed over 1024 that is <= 1% dimming and 2e-4 of white,
  worst case. (3) **CPU significance cull** (`dustCullInsignificant`, at the plumes' emission, on a CPU copy — never
  read the mapped list back): a worst-case bound per Gaussian (its largest column along any ray from the eye, in
  whitened coordinates: K sqrt(2 pi) (s0 + sMax / (Y sqrt(e))); x the billows' 2.5; light from the colour and every
  listed engine light), the faintest dropped while the SUMMED bounds stay < 2e-4 of white and 1% dimming (~60 in the
  droneship view: the dimming budget binds, an eye inside a long Gaussian can look down its length). Knockouts
  16777216 (tile cull + faint skip off) and 33554432 (CPU cull off) are the A/Bs. Measured interleaved in one live
  app, scenes paused (RTX 3070 Ti, 1600x900, cloud_march): droneship 8.1 -> 6.6 ms, era 2032 6.0 -> 5.0, day lift-off
  4.3 -> 3.4, night follow 3.3 -> 3.0; captures within the base-vs-base noise floor; composite at 128 registers,
  8.8 KB shared. Also cheaper in the loop: `eM` = e + tc^2 q (no extra matrix product), the Earth-shadow test only
  where used. **The bench's "+22 ms" for era / droneship was GPU contention** (another agent's app on the GPU: the
  same scenes measured +3.7 / +3.6 before this pass) — time the launch bench only with no other app running, and
  trust interleaved live A/Bs over single bench runs. `launch state` -> `dust_list` (counts by kind and cone width,
  `culled`).
- **Flames** (2026-10-19, issue 20261019_175126 shots 5-6): out of the model's engines (`engineGeomOf`: the exit plane
  and the radius the `engines` / `upper_nozzle` / `side_nozzle` cones span, cached per type in `LaunchState::engineGeom`),
  along -nose, a chain of three emissive Gaussians (core / flame / tail: 1.2 / 3.5 / 9 exit diameters x (1 + 2.5 x the
  thin-air factor) long, 0.75 / 1.1 / 1.8 exit radii x (1 + 2 x it) wide, 50 / 32 / 18% of the light), each centred far
  enough behind the exit that its forward tail stays in the bell (the old one was 950 m x 126 m, centred half its length
  behind the track point: it reached past the nose). Its light: unresolved, the point's on-screen energy (`toSky`, ~10
  mag over a surface's physical brightness); resolved (core 1.5 -> 8 px wide), the flame's surface brightness in the
  sky's units (pi x radiance per unit solar irradiance) x `kFlameCore` 30 (the magnitudes are tuned for points),
  geometric blend. The point (bloom, glare) sits 0.08 exit diameters behind the exit plane, on the engine cluster
  (2026-10-20, issue 20261020_181510: on the core Gaussian's centre it was ~33 m behind a Starship's exits in vacuum,
  ~300 px off the engines seen from behind and aside), its brightness capped where the flame is 4 px long and
  down to 30% once it is 30 px: glare at every scale, no white disc on the hull. A coasting stage's sunlit point sits
  at the middle of the stage (exit + half the model's length along the axis; it was the aft end) and is
  dropped once its mesh is up (floored at +10 mag it was still a -7 disc at 100 m). The night glow of the lit exhaust
  scales with the engines (12-330 exit diameters, 0.12 of the flame's light) and sits 2 of its lengths behind them.
  **Close up (2026-10-20, issues 20261019_222938 shot 2 / 20261019_222829, "a smaller glare with all 33 engines lit up"):**
  - The nozzles glow while a stage burns: `SatOrbitElems::engineGlow` (set with `fixAxis` for every rocket mesh slot,
    the selection's and the followed one) -> `GpuMeshInstance::earthShC.w`; sat_mesh.frag adds an emission (orange at
    the lip to white-hot up the bell) on materials with `lattice.w` = 1, which `packSatMeshMaterial` sets for the
    rocket models' `"soot"` (the bells' inner material). Render-only parts: no bloom, no glare. Each channel is capped
    at what displays orange (lip, x = 2.0 / 0.7 / 0.18) .. yellow-white (throat, 3.2 / 1.9 / 0.9) at the frame's
    exposure (`min(phys, cap / camPos.w)`, 2026-10-20): the night exposure (~5x the day's) saturated the physical glow
    to a white ring. (Not judged in a capture: from the harness's follow offsets the flame Gaussians fill the frame.)
  - The flame's Gaussians fade out as the eye comes inside one (its Mahalanobis radius 3 -> 1.5 sigma): from under a
    rising stage the tail's Gaussian lit every direction (a white screen). Resolved, each is held at its side-on surface
    brightness (x sP sqrt(q)), and seen from the exhaust side within ~40 deg of the axis thinned to 0.15, so the lit
    nozzles show through; `kFlameCore` is x0.18 at night (the night exposure is ~5.5x the day's: hundreds of times past
    white, a Gaussian saturated out to 3-4 sigma, a round blob several times the cluster's size).
  - The exhaust glow fades as the eye comes within 2-6 of its lengths; the point's flare blends geometrically toward 14
    (just past the glare threshold) as the flame grows from 4 to 30 px (it was ~2e4 at 200 m: a screen-wide bloom).
  - cloud_march.comp: the emissive light (flames, the glow, fireball trails) is hidden by cloud only where the cloud is
    NEARER than it (`eT` vs `tCloudMeanT`; a cumulus 2 km below a launch's glow was cut out of it: the report's "black
    blob"), and a texel with no cloud carries the nearest emissive / plume distance in its alpha (`tEmitNear`), so
    sat_sky.frag composites a flame in front of its own stage's mesh (the no-cloud marker hid it behind the hull).
    That distance is flagged in the alpha (see "Target A's alpha" under Clouds v2), and the mesh test is SOFT
    (`kM`: composite fully when nearer than the mesh, none past 1.25 x its distance + 20 m): the stage sits inside its
    exhaust, whose light-weighted distance is about the hull's, and a hard test drew black blotches on the hull.
  - Not built: clouds lit by a burning rocket (a cumulus beside a night launch stays moonlit-dark).
- **Ground clouds and the turbulent low trail** (2026-10-20, the user: "smoke trail effects (volumetric) on launch,
  dust kicking up from landing"; `buildParcels`' `groundCloud` / `landingCloud`, `launch::ParcelOpts`):
  - **Plumes are in the sky's units now.** The sky and the clouds use phase functions normalised to a MEAN of 1
    (`phaseR` / `phaseM` / `cv2HG`: 4 pi per steradian); the plumes' phases are per steradian, so every plume was ~13x
    too dim by day — a thick sunlit trail or a steam cloud was as dim as the horizon haze behind it, invisible from a
    few km. Their colour is x 4 pi by day (`nightBoost` = mix(10, 4 pi, dayness): the night value, 10, is the factor
    the twilight bells were tuned with, unchanged). The multiple scattering has a second term for optically THICK
    exhaust (`ms` + 0.17 (1 - exp(-0.25 tau^2))): ~albedo / pi for a white mass; thin trails unchanged.
  - **Lift-off**: while the stack is within hImp = 60 (flow1 / 1000 kg/s)^0.4 m of the pad (~200 m Super Heavy, ~90 m
    Falcon 9; at least 3 s, at most 20 s), every 0.75 s parcels of exhaust + deluge steam start on the pad's ground
    (`LPad::elevM`) and roll out: a Starship pad (water sprayed up through its steel deflector) 3 a step all round,
    70-150 m/s; other pads 1 round the side and most of it out of the flame trench's mouth 70 m behind the pad (-u) as
    a fan. Mass flow1 x 0.75 s x (1.3 deluge / 0.9) x (1 - h / hImp)^1.5 (floored 0.15); sigma0 22 m; they grow with
    their speed through the air (`growK` 0.12: d sigma / dt += 0.12 min(|v - wind|, 60 m/s)), rise (`buoy` 0.25 m/s^2
    fading over 90 s: hot steam), evaporate over 6-7 min; the wind carries them. `Parcel::ground` 1.
  - **Landings**: in a landing burn's last stretch (the stage within 80 (flow / 1000)^0.4 m of the ground: ~115 m
    Super Heavy, ~105 m the ship, ~50 m a Falcon-class booster) 3 parcels a 0.5 s spread from under it, 40-90 m/s.
    What they are follows the surface (`demElevCpu` > 1 m and `shoreDistCpu` < 0 = land): a pad (a tower catch: the
    launch pad for a booster, `landPad` for the ship) steam + exhaust, grey-white, 5 min; bare land tan dust
    (`Parcel::tint` (1, 0.84, 0.64), albedo 0.6, 8 min); the sea spray (white, kappa 14, gone in ~90 s; a droneship's
    deck sits in it). `Parcel::ground` 2.
  - **The low trail**: ascent parcels take `growK` 0.05 fading out by 6 km above the pad (the column thickens as it
    entrains); parcels within 30 km of the pad rest on ITS ground (`ParcelOpts::groundM`; was 3 m above sea level, so
    an inland pad's ground cloud could sink into the terrain). The growth is capped at 60 m/s of relative speed: a young
    parcel still moves at km/s, and uncapped the trail swelled hundreds of metres in a second.
  - **cloud_march.comp, below 3-12 km** (`lowK`): the billows are read in a field SHARED by every Gaussian there
    (EARTH-fixed: `cloudNoiseTex` at 2048/3 m and 2048/9 m cells, read at the point's ECEF offset from the terrain
    detail's integer-anchored 2048-m cell, `terrainAnchorRel`, so the cell index drops out of the repeating texture; it was
    P / 600 m with P Earth-centred in the EYE's ENU axes, which turn with the eye's position: the field slid with the
    camera, 2 km a second at the reporter's 2 km/s — issue 20261020_214200 "noise evolves with position"; checked by
    moving the eye 55 m with time paused: the billows' high-pass now correlates with the trail's parallax shift (0.16)
    rather than with the screen (0.06), the reverse of before), so the
    overlapping puffs of a ground cloud are modulated together into one lumpy mass instead of averaging their own noise
    into a smooth haze (per-Gaussian noise, which rides with its parcel, is kept higher up); the faint tails are cut
    (tau x smoothstep(0.02, 0.35, tau); also below the 0.01 noise threshold — left whole there they drew rings round
    every lit ground cloud at night); the lumps are lit by the field's change toward the Sun (`lumpL`, one more fetch
    70 m sunward) and darkened against the engine lights by their density (`lumpA`). Every plume Gaussian with
    tau > 0.01 is SELF-SHADOWED: its own optical depth toward the Sun (closed form, as the view's) from where its light
    comes from (the closest approach moved toward the eye by up to 1.5 sigma as the ray's optical depth grows: a thick
    puff is seen by its near surface), mix(exp(-tauS), 1, 0.4 .. 0.25 low).
  - **Sky light**: below 25 km the CPU adds the blue sky's light (amb x 0.18 (0.62, 0.78, 1) x albedo, the plume
    units) and, where that is more than 2% of the Sun's, Earth-shadows the Sun's part itself and passes the sky's share
    of the colour in ib.w's fraction (2 + share; below 2 = unread): the shader lights that share isotropically (`Lf`)
    with half the self-shadow. Above 25 km nothing changes (the bells keep the per-pixel terminator).
  - **Night**: the ground clouds take kessler's engine lights (`LpE`, `engineLightAdd`) like any plume, with the
    multiple scattering those lights were tuned with (`ms0`; with the thick term the steam saturated). The
    saturated orange-white ball a night lift-off cloud made from 2.5 km is addressed by the eye's adaptation to the
    engine lights (2026-10-20, below in "Rocket light on clouds": ~x0.5 there).
  - **Cost** (RTX 3070 Ti, 1600x900, Starship T+28 s from 2.5 km, interleaved in one run): GPU 11.25 / 11.11 ms with
    the new cloud_march vs 11.27 / 11.05 with the old (within noise); plumes off 10.60; with clouds 12.22 vs 11.96.
  - Harness: `launch next ... pad_dist_m=<m> [lead=<s>] [side=-1]` = on the ground beside the pad, looking at it,
    lead s before lift-off; `launch landing droneship=1` = a booster landing at sea, 30 m over the water `dist_m`
    aside; `launch state` -> `ground_clouds` {lift_off, landing, sample: kind, alt, sigma, tau, drawn colour, iso share}.
    `tools/harness/scripts/pad_smoke.satcmd` (Starship / Falcon 9 by day and night, a ship and a booster catch, a sea
    landing). Clear the clouds (`set clouds_v2.coverage 0`) and pause time after a jump.
- **Mesh glare needs a compact Sun image** (2026-10-19): sat_mesh.frag packs `meshGlareRoughK` (1 below roughness
  0.04, 0 past 0.10) with the photometric share into the scene target's alpha (`meshSharePack`: share 64 levels, flag 8;
  mesh_bloom.frag and mesh_refl_add.comp unpack it), and mode 3 (the viewer's glare source) multiplies its specular by
  it. Rough lobes (stainless 0.12, paint) reached the Sun-like radiance test at grazing incidence (Fresnel -> 1, the
  masking term's 1/(2 alpha)) along every edge and panel seam of a Starship in daylight: dozens of glares. Mirrors,
  OSR, dielectric film and solar glass (0.05: 0.93) still glare.
- **Local lights on the meshes** (2026-10-21, sat_mesh.frag; `SatelliteSim::MeshLocalLight`, `meshLightAdd`): a burning
  engine (from just behind its nozzles, 0.12 of the flame's length: `engineLightAdd`'s `meshPosEci`), and at night each
  flood of a pad (`padLightsOf` kinds 0 / 2, 0.16 suns m^2 ~ 2e4 cd, a 40-deg beam at the pad's centre 25 m up / a landing
  zone's centre) and of a landing ship (0.1, 45 deg at the deck) — the single cloud / haze light at a pad's or ship's centre
  has `meshK` 0. Lifted at night like the engine lights and with the eye's adaptation (`meshLightAdapt_`); Earth-fixed
  positions (last frame's list: the launch visuals run after `recordMeshScene`); the 8 most important to the camera in
  `GpuMeshFrame::lightPos / lightCol / lightDir` (528 -> 656 B). Diffuse + the material's specular, no shadow rays, not
  photometric (no bloom). The lightning pass's mode-2 ground term now skips mesh pixels (binding 6 = the mesh distance
  target, `writeLightningMeshDescriptor`): it lit them by the local up. Cost ~0.2 ms with the lights on (issue view).
  **Cheaper and in the info window (2026-10-21):** `fillMeshLocalLights(frame, originEcef)` picks the lights (the scene:
  the camera; the info window's 3D view: the viewed object, so a rocket on its pad at night is lit there as in the main
  view — Live light and a tracked subject only), and `meshLightMask` gives each instance the bits of the lights that
  can reach its bounding sphere above ~1/1000 of white at its exposure, an aimed flood only if the sphere is in its
  beam (`GpuMeshInstance::trans[1].w`, a float of the bits; 0 = none). sat_mesh.frag loops over those bits only and skips
  a light under ~1e-4 of white at the pixel before its specular lobe. Measured (launch_bench night_close: following a
  night Starship at 300 m, 1600x900): GPU 16.21 -> 15.46 ms, the launches' share 2.24 -> 1.60 ms (single runs while no
  other app ran — repeat before trusting the 0.1-ms digits). Harness `launch state` -> `mesh_local_lights`,
  `mesh_lights_scene`, `mesh_lights_viewer`. Still no shadow rays: the shadow accel bins occluders across the SUN, a
  point light would need the full occluder loop (the ISS: tens of ms), so local-light shadows are not built.
  **A light is a sphere** (2026-10-21, sat_mesh.frag): its diffuse term takes the sphere-light wrap
  `(n.l + sinA) / (1 + sinA)`, sinA = rs / sqrt(r^2 + rs^2) (specular still needs n.l > 0). An engine's light sits ~10 m
  behind the nozzles with rs ~10 m, and a flying stage's hull above it is at a grazing angle: as a point it lit almost
  nothing, so in the info window's 3D view a rocket at night was a black silhouette over its flame (main view too, close
  up). Floods (rs 0.5 m) are unchanged. Harness `launch state` -> `mesh_lights_viewer_list` (each viewer light's offset
  from the subject, I, and whether it is in the subject's mask).
- **The landing halo (issue 20261020_213739, 2026-10-21)** was the PLUME, not the haze: a droneship landing laid down sea
  spray (6x the exhaust's mass at kappa 14, growing 7 m/s) as a dome ~50 m round the booster, lit white by the burn and the
  floods. A ship's deck now gets a thin exhaust skirt (`landingCloud` surf 3: 1.2x the mass, kappa 9, sigma0 6 m, growth
  0.05, 50 s), and a landing burn's own trail parcels are at half the trail's cross-section (`kappaMul`). The haze term in
  lightning.frag mode 2 takes the light's core radius in quadrature at a third (`sqrt(d^2 + 0.12 r0^2)`): floored at r0 it
  was a flat disc 25-100 m across with a rim (the "haze halo at 300 m" following a stage).
- **Grid fins** (material `gridfin`, `GpuSatMeshMaterial::lattice.w` 2): a square grid without diagonals (member width
  1 - sqrt(1 - coverage)), 0.14-m cells (tools/make_rocket_models.py; render-only, so no photometry change), drawn SOLID at
  the grid's mean (plates + 0.3 x the cells) once a cell is under 3-6 px (`latSolid`, sat_mesh.frag): dithered they were a
  speckle at every normal distance. The soot glow tests `lattice.w` == 1.
- **The sea's reflection near a flame** (sat_sky.frag): the texels within an engine light's on-screen disc are dropped only
  where the flame DOMINATES (the composite there >= 3-10x the dimmest of four taps 2.5 disc radii out): clouds round a flame
  keep their reflection; issue 20261020_181339 shot 2's column stays gone.
- **Rocket light on clouds, haze, plumes and the ground** (2026-10-20; the glare pass's open item "a cumulus beside a
  night launch stays dark"). Every burn (ascent, boostback, entry, landing), a ship's entry glow and each pad's
  floodlights are POINT LIGHTS (`engineLightAdd`, SatelliteSimLaunches.cpp): intensity in suns x m^2 from the flame's
  magnitude at 100 km (`flameIntensitySunsM2`: mag 0 = 2.54e-6 lux, the Sun 1.27e5; Super Heavy -6 ~ 6e6 cd ~ 50),
  lifted at night by the MOON's lift (moonGain / 2e-6, ~2600x; physical by day, so nothing shows), x
  `launches.engine_light_gain` ("Rocket light on clouds", Off / x0.3 / x1 / x3). Pads: 0.5 (Starship 0.8), no ground
  term. Reach = where a lit white cloud is 2e-3 on screen; at most `kMaxEngineLights` (32), nearest lit region first.
  - **Delivery: the dust buffer's TAIL** (`kDustBuf` grew by 32 entries: entries 1024+ are lights in the 80-B
    `GpuDustCloud` slots, header word 1 = count, `engineLightsFlush` after launchFillVisuals; layout in
    `include/lightning_draw.glsl`). No new binding in cloud_march.comp.
  - **Clouds, fog, haze, ground: the lightning pass's mode 2** (`lightning.frag`, `params.w` = 2, `params2.z` = the
    count; the lightning set gained binding 4 = the half-res scene depth, 5 = the dust buffer). One full-screen
    triangle after the sky TAA, screen-blended, drawn whenever there are lights (with or without flashes). Clouds: the
    composite's distance per gathered texel, its opacity from a 3x3 TENT of the transmittance (gating per texel drew
    the outline as a staircase), 0.85 x opacity^1.5 / (r^2 + r0^2), x 0.35 with the light on the far side of the cloud
    from the eye (by height). Ground: albedo 0.15 x the cosine to the local up, behind the cloud transmittance, not on
    the hull (a cylinder up the nose axis). Haze: sigma(h) / 4 x the closed-form integral of I / (d^2 + s^2) along the
    ray, ending at the ground or (by opacity) at the cloud. Each term fades over the outer 40% of the reach (a hard
    cut drew a circle from orbit). Tried and dropped: shading by a cloud normal from dFdx/dFdy of the half-res
    distance (a 2-px crosshatch over the whole deck).
  - **Plumes: cloud_march.comp's `dustCloudsCS`** adds each light's irradiance on every plume parcel (x its albedo,
    isotropic phase + its multiple-scattering term): the low exhaust trail glows orange.
  - **Colour**: the flame's pushed 85% toward (1, 0.45, 0.15) .. (1, 0.38, 0.08) by soot (the first cut, 50% toward a
    paler orange, read beige over the city-lit cloud).
  - **The eye adapts to the lights at night** (2026-10-20, QA: following a Starship 310 m off at T+49 s the whole view
    was a saturated orange-white haze; the CLAUDE.md open item "a night lift-off cloud from 2.5 km saturates" is the
    same thing). The night lift (moonGain / 2e-6, ~2600x) is right for an eye adapted to moonlight; 300 m from a Super
    Heavy the eye stands in ~260 full Moons of the flame's light. `engineLightsFlush` scales every light's intensity
    by (1 + R / 2)^-0.75 (and its reach by the square root), R = the illuminance at the eye from all the lights in full
    Moons (sum of I / (d^2 + r0^2) / moonGain), only the night's share (mix by 1 - dayness), eased (`engineAdaptEased`:
    3/s toward darker, 0.5/s back). 310 m: x0.04; 1.5 km: x0.11; 2.5 km from a lift-off: ~x0.5; orbit / 10 km: ~1.
    `launch state` -> `engine_light_adapt`. Not adapted: the flame's own Gaussians (their night factor, kFlameCore
    x0.18), the point, the nozzle glow. **A cut snaps it** (2026-10-20): a sim-time jump > 2 s, the eye moving more
    than max(300 m, 0.2 x the nearest light's distance) in one frame (teleports, bookmarks, harness jumps), and every
    frame of a Go-to flight (`engineAdaptPrevEye/T`); before, the first ~0.5 s after a jump beside a stage were a
    white-out.
  - **A plume Gaussian's engine light is averaged over its depth** (cloud_march.comp: I / (r^2 + 1/q + r0^2), 1/q its
    variance along the ray: the young exhaust Gaussians beside a burning stage are hundreds of metres wide and were
    lit as if all their mass sat at the ray's closest approach), and the plumes' compositing distance (`tP`, in front of
    / behind the clouds) is LIGHT-weighted (a dim, thick ground cloud km away outweighed the bright exhaust 300 m off).
  - **The "bright ball" round a stage seen from 300 m was mostly lit TWICE** (2026-10-20): the plume texels' alpha
    carries the plume's distance (`tEmitNear`, above), and lightning.frag's mode 2 took them for cloud at that distance
    and lit them again (0.85 I / r^2 on the plume's opacity) on top of cloud_march's own engine light on the plume.
    That is also what drew far horizon clouds as DARKER half-res blocks inside the glow: their texels kept their own,
    far distance (lit by nothing) beside plume texels lit as a cloud 300 m off. Mode 2 skips flagged texels now.
    Then: each plume Gaussian's engine light is SELF-SHADOWED like its sunlight (closed form along the segment from
    the engine to the Gaussian's lit point, floor 0.25), so a thick young column glows on its side toward the flame
    instead of over its whole 6-sigma footprint; and a Gaussian the EYE is inside thins (Mahalanobis 3 -> 1.5 sigma,
    to 10%; in the QA views the eye was in none). Mode 2's cloud term lights the cloud at a [1 2 1] tent of the 3x3
    texels' opacity-weighted distance (`tLs`): per gathered texel, a strongly lit deck's outline and gaps were a
    staircase of 4-8 px steps. What remains round a stage from 300 m is the lit haze (1/d around a point light,
    physical single scattering) and the lit column: `tools/harness/scripts/follow_visuals.satcmd`. Cost (RTX 3070 Ti,
    1600x900): cloud_march +0.05 ms (T+15 s, 6 km from the pad) / +0.25 ms (following at 300 m), the rest within noise.
  - Measured (RTX 3070 Ti, 1600x900, Starship at T+15 s under broken cumulus, 6 km from SLC-37): +0.26 ms in the draw
    bucket, plume lighting within noise. Physically the lit deck is wide: a Super Heavy lights cloud 6 km away like a
    full Moon (the user's call whether x1 is too much).
  - Harness: `launch state` -> `launches.engine_lights` / `engine_light_gain`; scenes `harness_runs/englight/r7.satcmd`
    (SLC-37 at night: `drift phase=2.86` broken cumulus, `5.2` overcast, `clouds_v2.coverage 1.6`; air, orbit,
    `launch landing booster=1`). Open: no cloud-surface orientation, no in-cloud diffusion for a light inside a cloud
    (lightning's boltGlow does that), no terrain shadowing, the half-res outline can still show where the light is strong.
  - **The sea mirrors them from where they are** (2026-10-20, issue 20261020_181339 "Rocket Ocean Flare"; sky set binding
    32 = `kDustBuf`, written by `writeCloudsV2ConsumerDescriptors`): sat_sky.frag, after the Sun glint, takes each engine
    / floodlight / ship light through the same Beckmann lobe with its direction FROM THE SEA POINT and its irradiance
    I / (d^2 + r0^2) there (suns), hidden by the eye's cloud composite in front of the light when it is on screen. Until
    then the only reflection of a rocket was the reflected cloud composite (the flames, the exhaust glow and the lit
    plume are its emissive texels), looked up at the mirrored DIRECTION, i.e. as if at infinity: from 4.9 km up, 3 km
    from a Starship 0.9 km above the eye (T+46 s), a glitter path ran from the horizon down under the flame, where the
    real mirror point is 74 deg below the horizon (horizontal distance D h / (h + H) = 1.35 km; a source at infinity at
    the rocket's 17 deg elevation would mirror at 17 deg). That lookup now drops an EMISSIVE-flagged texel (no cloud,
    `cloudAlphaIsEmit`) whose light, at its stored distance, misses the sea point's reflected ray by more than 2-6 deg
    (`rEmitBad`). **Shot 2 (same day, "still broken", the user's build had the fix):** a flame in front of the horizon's
    cloud is composited into that CLOUD's texel (alpha = the cloud's distance, unflagged), so the test never saw it;
    the lookup now also drops any texel within the flame + glow's angular size round an engine light's own place on
    screen (>= 3 deg, ~600 m) when the reflected ray misses that light's real position seen from the sea point (real
    clouds right round a flame lose their reflection there). Reproduced with `report load` shot 2 (its own settings):
    the column is gone, the true mirror point (D h / (h + H), ~75 deg down) is below the frame. Far rockets (40-100 km) keep a glitter path from the horizon down,
    now from the light glint. Satellites' `OceanGlintBuf` glints are unchanged (at hundreds of km the infinite-distance
    geometry is off by < 1 deg). Cost within run-to-run noise (sky pass 11.9-12.8 ms with the lights vs 13.0 without,
    1920x1009). The bloom ellipse under the flame (the flame's tail Gaussian + the exhaust glow, 2 of its lengths behind
    the engines) and the faint column to the pad (the low trail lit by the engine light; gone with "Rocket light on
    clouds" Off) are the exhaust, not reflections. Open: lightning.frag's ground term lights the sea as a 0.15-albedo
    diffuser (water's body reflectance is ~0.02-0.05).
- **A hull inside its own exhaust shows no cloud from behind it** (2026-11-10, issue_20261110_184611 "Odd transparency on
  rockets", a Starship at 58 km at sunset, the eye inside the plume). cloud_march.comp drops a resolved cloud whose
  distance is past the texel's surface (2026-10-21); with full-res clouds cloud_v2_combine.comp then added each pixel's
  own resolved cloud back on top (its derivatives), the far deck behind the hull, with the cloud's distance as the alpha,
  so sat_sky.frag's per-texel mesh test also dropped the exhaust in front there: dark, cloud-shaped, half-res-stepped
  patches over the hull that slid as the eye moved (knocked out by `plume_gain` 0, `coverage` 0, `cdc_skip` 0, half-res
  clouds or the full-rate march; not by reflections or the sky TAA). cloud_march now flags such a texel (DB.a -1) and the
  combine adds none of its cloud and drops its distance. The report's view: the hull strip's luminance 22..117 (sd 32)
  -> 58..118 (sd 15), the patches gone in every frame of the reporter's trail. What remains is physical in the sim's
  terms: the hull veiled evenly by the exhaust it sits in, as the sky around it is (`plume_gain` decides how much).
  Only texels the 2026-10-21 guard already empties change (a cloud behind a mesh or a ridge); cost a branch.
- **Sound**: each burning rocket within 150 km on a thunder voice (`launchSounds`, called from `thunderStep`): the
  retarded time, Earth-fixed (the air carries it), level ~ sqrt(flow) / distance, thinning with the air at the rocket.
  The retarded time is the LATEST emission tau with (T - tau) = |P(tau) - eye| / 343, found by a scan back over 440 s
  (150 km of sound) in 2-s steps for a sign change either way (a booster's supersonic descent arrives in reverse
  order), then bisected; nothing arriving = silence. (2026-10-19, issue 20261019_175639: a fixed-point iteration that
  did not converge for a fast, distant stage alternated between the stage and the pad, so an upper stage 1200 km
  downrange was heard "burning" at full level 300 m from its pad minutes after it left.) Rocket voices are their
  own kind in the thunder synth (pushEvent's 5th value = 1, `rocketTick`): a low roar (brown + pink noise
  low-passed at 2200 / (1 + km / 1.8) Hz, under a 62-Hz sub layer), Mach-wave crackle (one-sided spikes, 350 Hz-9 kHz,
  fading out by 40 km), a punch when the roar first arrives (x3 over ~1.4 s with a 25-ms attack, a burst of crackle,
  a thump falling from ~70 to 30 Hz) and a send into an FDN echo (t60 3.4 s: the roar off the ground and landscape).
  Measured offline (harness `audio record ... solo=thunder`, `harness_runs/rk` on the kessler branch): 300 m from the
  pad ~-23 dBFS while the roar lasts, 25 km ~-31, sub band dominant (centroid ~35-40 Hz). How it sounds is the
  user's to judge.
- **Selectable** (2026-10-19): rocket stages, Starship's ship coming home and every re-entry fireball (retirees, upper
  stages, the Kessler re-entries, small fragments) are TRANSIENTS, not records: the code that draws their heads lists
  them each frame (`SelTransient`, `addSelTransient`: a stable key, ECI position / velocity / acceleration, the ENU
  direction, the drawn flare; a rocket is listed even coasting unlit so a tracked one stays selected, pickable only
  once drawn). A click picks the nearest within 10 px (`pickSatelliteAt`, before the GPU list's nearer satellites);
  the selection is the reserved LAST launch slot (`transientSatIdx()`, its chunk kept out of the allocator,
  `typeIdx = UINT32_MAX`: no model, so Info / Go to / Trace hide and `selActionAvailMask` = Track), whose elements are
  `SatOrbitElems::fixedState` (quadratic extrapolation from the frame's state, clamped to 30 s), rewritten each frame
  (`selTransientUpdate`, after the visuals). Label from `transientLabel()` (vehicle, stage, burn, T+, altitude, speed;
  "Re-entry: <satellite>"), magnitude from the drawn flare (as seen from the CAMERA: following a Super Heavy 310 m off
  it reads ~-18.5, the physical value of a -6-at-100-km flame there; the Info window's MAGNITUDE tile is the ground
  observer's, the flame carried there by 1/r^2 + its extinction, since 2026-10-20 — it read "Dark", the model's
  sunlight). Since 2026-10-20 the label reads "on the pad, T-9 s" before lift-off, a returning ship's "belly-flop
  descent" / "flip and landing burn" / "caught by the tower" / "splashed down" after `tShipDesc` (it said "falling
  toward entry" and "deorbit burn"), and the speed is over the ground below 80 km, inertial above 150 km (a stage on
  the pad read 0.41 km/s, the Earth's rotation); the Info STATUS row the same. Not drawn for 2 s of sim time: deselected. The selection
  panel's second line (`selInfoLine[1]`, 160 wide since 2026-10-19 — labels were cut at 40) describes the object:
  "Rocket stage (starship_upper)", "Starship ship, entry", "Re-entry fireball", and for a launch record
  `launchStatus()` ("<type> - raising to slot, 412/565 km" / "parking orbit" / "in the dispenser" / "deorbiting",
  refreshed every frame). It printed the constellation's type, and the transient slots have none: a bare "?" under
  every rocket and fireball (the user's "? underneath it"). Harness
  `launch select kind=rocket|ship|fireball|retiree_reentry|stage_reentry|kessler_reentry|fragment`, `state` ->
  `selection.transient`. Go to (follow) works on a rocket (it has a mesh). **A click on a rocket's MESH picks its
  transient** (`pickSatelliteAt` maps a rocket mesh slot to its key): it used to select the mesh slot itself, whose
  elements are only last frame's state, so a selected or followed rocket froze there for good (the user's ship "still
  up here" seven months after it had re-entered) and, followed, was drawn twice (its live self in a rocket mesh slot
  beside the followed copy). The followed transient keeps updating (and is dropped with the follow when it is gone)
  even with the selection cleared, is never also drawn in a rocket mesh slot, and is listed at any range.
  **Following a transient** (2026-10-19): the followed one has a slot of its own (`followSlotIdx()`, the reserved
  chunk is now 6 rocket meshes + the followed transient + the selection), independent of the selection, and is
  evaluated from the launch model at the frame's exact time (`transientExactState`: no stale extrapolation at any
  warp; the list's axis is kept). Along a mission it hands over (`transientContinue`): the stack to its UPPER stage
  at staging (the booster is another object), a returning upper stage to its ship entry (same key, kind 2), a
  deorbiting stage to its re-entry fireball; the selection does the same (`transientSuccessor`). When nothing
  continues it, follow ends in place (`stopFollowHere`).
  **Handovers seen from the Info window and the selection** (2026-10-19): (1) the 3D view switches model with its
  subject (`viewerSyncModel`, called from `updateViewerView` AND `recordModelViewer`, since the selection's slot is
  rewritten between them): same camera direction, distance scaled by the models' sizes, env probe kept (it belongs to
  the index); a subject with no model (a fireball) keeps the last model posed at its position (`viewerKeepModel`)
  and its SKY POSITION / magnitude readouts come from its state, not dashes. Before, `typeIdx != viewerType` made the
  view drop to "placed" mode: the old model above the observer. (2) While Go to rides the selected transient, the
  selection mirrors it through every handover follow makes (`selFollowKeyPrev`, top of `selTransientUpdate`); an
  expendable stage's selection takes its kind-4 fireball as soon as it is listed (its listing ends before
  `tStageGone`) and waits 5 s longer for it. (3) A train member's record that ends hands the selection AND the 3D
  view to its roster slot, a retiree's selection to its re-entry once listed (`launchSlotEndedSelection` from
  `hideSlot`, `selWant*`). (4) The handover jump is measured at ONE time (`transientPosAt`: followTrCur alternates
  between the list's time and the frame's), the free aim turns with the eased jump so the object stays where it was
  on screen (`followHandViewA/B` in `updateFollow`), and the upper stage's blend from the ground track into its
  inertial plane starts AT staging from the booster track's point (`rocketPos`; it began 60 s early on its own track:
  a ~4-km jump at separation, the rocket left the frame). Staging hands over across the stack's upper part (the
  upper stage starts where it sat on the stack: Starship 72 m above the stack's aft end), eased; the ship's entry still
  jumps ~0.5 km (its own entry model) and an expendable stage's fireball ~2.7 km (the Kessler fireball placement),
  eased with the object held on screen. Harness `state` -> `selection.panel_lines` / `viewer_sat`.
  Verified (harness_runs/gk/mission.satcmd): a Starship
  followed from T+8 s through staging (eased 49 m), the upper-stage burn to 350 km, coast and deployment, the fall,
  its ship entry (eased 5.2 km) and the end, left as a free observer at 45 km.
- **Kessler** (L6): launches stand down at the CME; by default for good ("After the Sun's Fury": Stand down /
  Resume, `launches.after_storm` 0 / 1; Resume = three weeks later). The trains would not die anyway (they are not
  engine objects), but nobody launches replacements into shells being ground into debris. A slot dead before its swap
  gets no replacement and no retiree. Trains and retirees are NOT engine objects (they do not collide); a slot killed by the cascade is not
  refilled.
- **Cost** (RTX 3070 Ti, 1600x900, a Vandenberg twilight launch's bell filling the view, 320 Gaussians): cloud_march +0.9 ms, CPU
  0.3 ms at 1x, 0.6 ms at 1 d/s. ~2600 planes, ~1200 live batches, ~15k train and ~18k retiring satellites at the default lives.
- CPU bucket `launches` (harness `perf`, Performance tab "Launches"; the visuals run inside the dust fill and are moved
  out of `kessler`). Its parts are in harness `launch state` -> `cpu_split_ms` (eased over 30 frames: window, live_list,
  freeing, writes (+ writes_member_records, due_evals), visuals (+ plumes, plume_builds, plume_sort_emit, reentries),
  pad_lights). **CPU pass (2026-10-20, Twilight Starship bookmark, ~5300 live batches, 1600x900):** 2.04-2.26 ->
  1.24-1.28 ms at 1x, 3.8 -> 3.4 ms at 1 h/s (plume builds ~3 ms there, unchanged). `launch check` and the launch
  state's counts (trains, retirees, slots, chunks, rockets, plumes) identical before / after on the same tree, at 1x and
  at 1 h/s. What changed, each exact: (1) the window walk (every plane, a hash lookup per batch) is reused while sim
  time stays within 300 s of it (`walkList`, `walkT`, `batchEpoch` bumped by any erase that may hit a listed batch);
  "listed now" is `LBatch::inWalk == walkFrame`, not `touched` (the Missions code bumps `touched` of batches it builds);
  (2) the freeing pass runs over the batches holding chunks only (`LaunchState::chunked`), and is skipped while t stays
  inside the interval in which no chunked batch's need changes (`freeLo / freeHi`); the erase pass is every 64th frame;
  (3) the record writes skip a batch from a compact copy (`liveT`, `liveCh`) when it is outside its raising and falling
  and holds no chunks, or while t is inside the interval its last look found nothing due (`liveSkip`, invalidated by a
  release and every GPU reset); nobody-can-be-up batches (before the first release, after the last re-entry) no longer
  rebuild their member flags each frame; (4) launchFillVisuals loops over `visList`, the live batches whose rocket /
  plumes / stages (tL - 600 s .. tVisEnd) or retiree re-entries (tA .. last + 200 s) are within the walk's margin;
  (5) plumes: one extinction column for the three colour channels (linear in k), the Earth's rotation once a batch,
  `std::partial_sort` of only the drawn Gaussians + 1, the candidate vector kept between frames. Left: the plumes'
  per-Gaussian work (~1 ms with the bookmark's ~700 candidates incl. the pad smoke) and the plume builds under warp.
  **Hitch pass (2026-10-20, `harness_runs/hitch/`, see GPU Performance Profiling "Frame profile and hitches"):**
  (1) **plume parcels are built on a worker** (`LaunchState::parcelJobs`, `std::async`, at most 4 in flight, from 2 min
  before lift-off; the batch copied in, the catalog / pads / tracks read: they never change after load; a
  `launchInitBlock` or the state's destruction joins them): `buildParcels` was 80-190 ms on the main thread the frame
  a launch first mattered (1 min/s - 1 h/s); a plume asked for after lift-off appears a few frames late. (2) **New
  batches are built in parallel** in the walk (collected first, then: their candidates, the rivals' day buckets they
  need, the builds, each `par`allel; `candidate()`'s cache under `candsMx`, a shared_mutex), at most `buildCap` a frame
  (follows the measured cost: ~8 ms of building; the walk is redone until complete). (3) **Pad deconfliction reads a
  day index** (`rivalBucket`, `LaunchState::rivalIdx`: per site and day of candidate lift-off, every batch of the
  planes flying from the site whose candidate is there that day): each build enumerated every plane flying from its
  site (~2000 at a Starship pad), ~1 ms a build. Same rank, same schedule (`launch list` / `padstats` identical before
  and after); `pad_rivals` now counts the rivals within the 90 min. (4) **`planShipLanding` skips exactly** the 30-s
  steps that cannot reach a Starship pad (its end moves over the ground at most |nPark| + the Earth's rate: from a step
  whose nearest pad is farther than acos(0.98), it skips that many steps; same grid, same answer). Load: the first
  frames were 6 x ~130 ms of builds; now one ~20-30 ms.
  **The background build (2026-10-21b, `LaunchState::BuildJob`, `startBuildJob` / `applyBuildJob`).** The ~8-ms-a-frame
  walk budget (hitch pass 2) made a time jump slow to fill: after `time set 2032-08-27` the walk had 8.5k of 9.7k
  batches after 480 frames at ~11 ms CPU a frame, and each bookmark Go took 250-300 frames at 9-12 ms. Now, when a walk
  finds more than 48 missing, it sorts them by urgency (|arrival - mean raise - t|: the launches nearest now first),
  builds the first 16 itself and hands the rest to ONE worker thread (`std::thread`, below normal priority; its
  `buildMissing` helpers use all cores but two, `gBuildBg` thread_local), which builds them in chunks of 64 then 256 and
  publishes each by an atomic `done`; every frame `applyBuildJob` (start of the walk, <= 2 ms) moves the finished
  prefix into `batches`. Keys in flight (`jobKeys`) are not built on the main thread; the walk stays incomplete
  (`walkOk` false) until the job is in; with a job in flight the walk builds at most 8 itself (4-ms budget) and past
  1 h/s of warp none (the first 16 neither: every frame is a new stretch of days whose rival buckets are cold, 20-60 ms
  on the main thread). `SATLIGHTSIM_LAUNCH_BG=0` turns it off (A/B: `launch list hours=72` / `launch padstats` after a
  2032 jump and a bookmark Go identical on and off, `harness_runs/fill/ident.satcmd` + `cmpid.py`). The job reads the planes,
  catalog, roster (`satOrbits`, the Kessler roster words) and the locked candidate / rival caches, writes only its own
  output: `launchInitBlock`, `crewSync` (a station's fate changed), `kesslerRestart` and `buildOrbits` join it first
  (`LaunchState::joinJob`, `SatelliteSim::launchJoinBuildJob`), as does `launchDestroy`. Same batches (buildBatch is pure).
  Measured (RTX 3070 Ti, 1600x900, `harness_runs/fill/`, fill.satcmd + parse.py): 2032 jump all 9706 batches in the
  first 40 frames (background 9690 in 1.1 s), launch CPU 3.4 ms over those frames then 1.9; bookmark Go 1/2/4/7/8
  complete in the first 40 frames (5-6k batches in 0.2-0.5 s), 1.3-2.3 ms over those frames (were 9-12 ms for 250-300
  frames). `launch state` -> `batches_cached`, `walk_complete`, `build_job` {items, built, applied}, `build_jobs_run`,
  `last_build_job`. **Plume skip:** a plume candidate whose priority is under 0.25 x last frame's cut is not finished
  (no colour, no rim); exact while this frame's cut stays above that margin, else the next frame skips nothing
  (`plumeCutPrev`): Twilight Starship 1.77 -> 1.47 ms at 1x (1801 candidates, ~1030 skipped; `launch state` ->
  `cpu_split_ms.plume_skipped`). The remaining plume cost is the per-parcel state (`Parcel::at`) of every parcel.
  **After a jump, and in the Missions window (2026-10-24, perf4-cpu):** a walk whose time is an hour or more from the last
  walk's (a Watch, a bookmark, a typed time) hands EVERY missing build to the job, however few (the first build of a day
  asks for its rivals' buckets: up to ~7-25 ms each on the main thread; 16 synchronous builds were ~20-30 ms of the
  landing frame); the rockets of that moment appear a few frames later. Otherwise a big set keeps 4 (was 16) for the
  frame. The Missions window's own refresh (`launchUpcoming` with a budget) builds nothing on the main thread: its
  missing batches go to a worker of their own (`LaunchState::UpJob`, joined by `joinJob`), the rows waiting for it
  (after the Sun's Fury Watch its searches were 25-30 ms a frame for ~4 frames); a forced refresh (a click, the harness)
  builds on the main thread in pieces sized by the measured cost of a build. A job's batches are marked touched as
  they are applied (the 64-frame erase pass could drop them before they were asked for).
  **The stand-down boundary (2026-10-21d, `standDownArrival` / `standsDownAt` / `maxVehicleLeadS`).** After the CME
  (after_storm 0) every launch stands down, yet the walk kept building the batches entering its window: at 1 week/s
  4-8 ms of `launch.buildBatches` a frame, plus the background build's apply, 11-27 ms in the worst frames (D,
  `harness_runs/standdown/`). A lift-off is never earlier than arrival - raiseDays[1] - the catalog's largest
  t2 + deployAfter (candidate() searches forward from tA - raise - lead; pad ranks and the fast-dock pass search only
  move it later), so a batch arriving after epoch + that is stood down whatever its pad: the walk's (and
  launchPrewarm's) window ends there (`hi = min(hi, standDownArrival)`), buildBatch stands such a batch down before
  its candidate, a candidate past the epoch stands down before its rivals, and buildMissing asks no rival buckets for
  one. Only `B.why` can differ (a post-CME batch reads "stand down" where it read "no site" / "pad taken"); every valid
  batch is the same. `SATLIGHTSIM_LAUNCH_STANDDOWN_SKIP=0` turns it off: `launch list` / `padstats` / `why` across the
  boundary (2037-05-01, 07-10) identical on and off (`harness_runs/standdown/ab.satcmd`, `parse.py`). Measured (D,
  frames over 33 ms): worst `cpu.launches` off 6.8 / 17.5 / 23.1 ms at 1 h / 1 d / 1 w per s, on <= 4 ms;
  `cpu_split_ms.window` 1 d 2.4 -> 0.65, 1 w 1.9 -> 0.2, 1 mo 0.6 -> 0.03; batches cached 7000 -> 4000. Still ~1.2 ms
  of record writes from 1 min/s to 1 d/s (the retirees deorbiting after the CME). Resume (after_storm 1) is not
  shortcut: its end depends on the ranks.
- Settings `launches.enabled / life_scale / plume_gain / rocket_gain / after_storm`; **Settings > Launches**
  (2026-10-21, issue 20261021_175638: the Missions window's Controls tab moved there; `buildSettingsLaunchesTab` = GRAPHICS first:
  ROCKETS AND EXHAUST (`buildLaunchGraphicsSection`) and GRAPHICS: DEBRIS AND RE-ENTRIES
  (`buildKesslerGraphicsSection`), then LAUNCHES and THE SUN'S FURY; Constellations keeps a pointer). **The exhaust sliders** (slots 313-320, `marksPreset`
  false, all 1 = the look before them; "Reset exhaust to defaults"): "Plume opacity" `plume_gain` 0-5, "Plume brightness"
  `plume_brightness` 0-5 (x the scattered colour, addPlume), "Plume width" `plume_width` 0.25-4 (x every Gaussian's radius,
  same cross-section), "Plume billows" `plume_billows` 0-3 (cloud_march.comp's modulation contrast via DustBuf `dustPad2`;
  the CPU significance cull's billow bound scales with it), "Plume growth" `plume_growth` 0-5 (x the turbulent growth the
  parcels are BUILT with: `LBatch::plumeGrow`, a change rebuilds the plumes in view), "Ground clouds" `ground_cloud_gain` 0-5
  (x the lift-off / landing clouds' cross-section), "Flame brightness" `rocket_gain` 0-10, "Rocket light on clouds"
  `engine_light_gain` 0-10 (the choice rows they replace are gone); harness
  `launch ...` (docs/HARNESS.md: `launch next vehicle=`, `launch select kind=rocket|booster|upper|...`); default
  bookmark "Twilight Starship Launch"
  (`tools/harness/scripts/default_bookmarks.satcmd` #8).
- **Crew rotations** (2026-10-19): a program with a `crew` block (`Program::crew / spacecraft / people`) flies people to a
  station — a constellation of ONE, which `buildPlanes` now accepts for crew programs — every `life_years`, which for
  crew is a cadence NOT scaled by the design-life setting (`progLifeS()`, the one place every refresh period comes
  from). `raise_days` is the TARGET for lift-off to docking (the phasing below sets the real docking, `B.tA` =
  `crew.tDock`); no train, no retiree (`B.mem` empty). Programs: ISS (Crew
  Dragon on `falcon9_dragon`, 39A / SLC-40), Tiangong (Shenzhou, `long_march_2f`, the new site `jiuquan_crew`,
  azimuth 80-200: the old Jiuquan sites' 120-200 cannot reach 41.5 deg), the commercial stations (Haven-1/2, Axiom,
  Starlab, Orbital Reef: Crew Dragon), ROS (Soyuz MS, `soyuz2`, Baikonur), Bharatiya (Gaganyaan on `hlvm3`, the
  human-rated LVM3 added 2026-10-20 — appended to the vehicle list, so no other program's schedule moved): two flights
  a year each, ~18 a year. The vehicles have meshes (`tools/make_rocket_models.py`; the shroud with its escape tower,
  the spacecraft on top of the upper stage once it is gone). Crew launches stand down after the Sun's Fury like the
  rest. Cargo ships fly too (2026-10-21: "Visiting vehicles" below). **A lost
  station gets no more crews** (2026-10-20, issue 20261019_231126): `buildBatch` refuses a crew launch whose station
  is dead or ended (min of its two roster words) at or before lift-off (`launch why`: "station lost"); a flight
  launched before reports "The station is destroyed (<date>); rendezvous called off" in Ongoing. The cached batches
  were built against the station's word at build time, so `crewSync` (launchUpdate, launchUpcoming) rebuilds a crew
  plane's batches whenever that word changes (a breakup, a rollback, the worker reaching a cascade kill): the schedule
  is a pure function of sim time and the event log (as mirrored: a cascade kill the worker has not reached yet still
  shows its flights). Harness `missions state` -> `stations[].fate` / `fate_hours_ago`.
- **Crew spacecraft fly as their own objects** (2026-10-20, issue 20261020_214759: "Crew Dragon due to dock in 1 min;
  the progression went straight to the second stage's deorbit; I cannot select the capsule"). `LCrew` in each crew batch
  (`LaunchImpl::buildCrew`, a pure function of the batch; `crewPos` / `crewAttitude` closed forms of t):
  - **Separation** at `B.tD`: the spacecraft leaves the top of its upper stage (launches.json `spacecraft_dock_m`, the
    model's origin = its docking port, checked by `make_rocket_models.py --check`) and drifts ahead at 0.4 m/s; the stage
    becomes `_upper_empty` (`upperStageType`, Dragon included) and flies its own deorbit.
  - **Phasing** in the station's orbit plane (`Us/As` at tSep; the stage's plane mismatch faded out by the first burn):
    a coast, a Hohmann raise to a phasing orbit `rP` (burns tB1 / tB1e), a wait there, a Hohmann to the station's
    altitude arriving 5 km behind it (tB2 .. tArr). rP is SOLVED (bisection on the catch-up) so the arrival lands as
    near the program's `raise_days` as the station's lead (+ whole turns) allows; with too short a time it waits in
    the parking orbit (`skipRaise1`). A fast track (Soyuz, Progress, Shenzhou, Tianzhou) picks its lift-off pass for
    the phase (below: "Visiting vehicles"). Each burn is spread over `kCrewBurnS` (240 s) about its impulse time: the
    path before it and the Kepler path after it (both defined either side, `crewTransfer`) cross-fade by a smoothstep,
    and the last (the circularisation at tArr) into the approach's first leg (`crewFarPos`; `raw` = without that
    blend, where the approach starts). The first cut changed the velocity in one step: ~84 m/s^2 in a 1-s second
    difference.
  - **Approach** in the station's body frame (`crewStation`: Z nadir, X along the velocity — the stations' own
    two-vector law, so the port lands where the mesh draws it; `LCrewSeg`): from 5 km behind to 1 km on the line toward
    the port (30 min), round onto the port's axis at 1 km (an arc about the port; opposite directions go round BELOW,
    the R-bar side: a Dragon comes up from under the ISS to a forward port), along the axis with holds at 400 m (15
    min), 200 m (15 min, the go / no-go) and 20 m (10 min), contact (0.05 m/s). Cubic Hermite / smootherstep legs:
    stops at every waypoint, no jump at any seam (`launch check` -> `crew_accel_max_mps2`).
  - **Docked** at the station model's port (SatModel `dockPorts`, JSON "docking_ports", written by
    `tools/make_station_models.py`'s `dock()` / `port(..., dock=True)`: ISS IDA-2 Harmony forward + IDA-3 zenith,
    Tiangong node forward + nadir, Haven-2 fore / aft, Axiom fore + zenith, Orbital Reef fore + nadir, Starlab fore + +Y,
    one on Haven-1 / ROS / BAS). Two ports: crew k docks at port k mod 2 and stays until the NEXT crew's nominal arrival
    + 5-7 days (a hand-over, both docked); one port: it leaves 2-3 days before. Docked = the station's position + R x
    port, attitude fixed in the station frame (+Z into the port). **The visiting crew vehicles drawn as render-only
    greebles on the station models (the ISS's Dragon and Soyuz, Tiangong's Shenzhou, the commercial stations' Dragons,
    ROS's Soyuz) are gone**, and so are the cargo ships' (2026-10-21: the sim flies Progress and Tianzhou there).
    The arc onto the docking axis carries the frame facing the port round the arc (rotated about its axis, `arcFrameB`
    in `crewAttitude`), its roll easing into the axis frame over the next leg's first 300 s: rebuilt from an "up"
    reference each moment it rolled 180 deg in a second on some arcs (Haven-1, ROS, BAS, Orbital Reef).
  - **Return**: back out to 20 / 200 m, a 40-min departure onto a circle 2 km under and 3 km ahead of the station, a
    10-min deorbit burn (timed in 12-min steps over a day so it comes down at SEA for Dragon / Gaganyaan, on LAND for
    Soyuz / Shenzhou — `crew.lands`, launches.json `"lands"`; the water map saturates at ~19.5 km, hence +-15 km), the
    trunk / service module gone 15 min before the entry interface (model `_cm`), a 9-min entry (plasma glow + wake as the
    ship's), drogues at 10 km, mains at 3 km over the ground (model `_chute`), its speed over the ground eased out
    (`chuteV`), splashdown / landing, drawn 40 min more. Attitude: prograde free-flying (as on its stage), port toward
    the port on the approach, the heat shield forward from 10 min before the burn, upright under the chutes. Turns near
    180 deg (a forward-port capsule turning prograde, prograde to retrograde) go about the local up (`crewSlerp`'s
    `ref`): the shortest way flipped sides frame to frame (178 deg/s in the first cut).
  - **Models**: `tools/make_crew_models.py` -> `rocket_capsule_{dragon,shenzhou,soyuz,gaganyaan}[_cm|_chute].json`
    (+Z out of the docking port, origin at its contact point, the three variants sharing it; arrays deployed; Dragon's
    nose cone hinged open; render-only canopies), loaded as rockets (the `rocket_` prefix: `rocketTypes`, the accuracy
    gate's load step). Program's crew block: `"model": "capsule_dragon"`, `"lands": true`.
  - **A transient** (kind 8, key batch << 3 ^ 3, `crewFill`): listed by `launchFillVisuals` from `L.crewList` (the live
    crew batches flying or docked, rebuilt with the window walk; the walk's window reaches back a refresh period + 14 days
    for crew programs) within 4000 km or when selected / followed; meshed in the rocket slots; a full attitude
    (`hasAtt`, turn rate by differences); a faint sunlit point that gives way to its mesh, a small warm point in its
    burns (nozzles lit: `engineGlow`), the entry's glow and wake. **Handovers**: the upper stage passing tSep becomes the
    spacecraft for follow AND selection (`transientContinue`; `selTransientUpdate`'s `crewSep`, the model places it if
    the list does not hold it); an upper stage picked after tSep stays the stage; back in time the spacecraft is its
    stage again. Label: phase, metres to the port, "docked at <station> (<port>), undocks in N days".
  - **Milestones** (`missionBatchMilestones`): the spacecraft's whole timeline is the PAYLOAD role (lift-off .. SECO,
    separation, phasing burns, arrival, ellipsoid, axis, holds, soft capture, undocking, departure, deorbit, trunk /
    module separation, entry interface, drogues, mains, splashdown / landing); the upper stage after the separation is
    `kMsStage` (its deorbit and re-entry). `missionAttachAt` puts the selection / Go to on the spacecraft past tSep
    (`launchRocketTransient` + `transientContinue`), on the stage for kMsStage. Missions > Ongoing: a crew flight stays
    listed until it is home (status = its phase; Select = the spacecraft, `launchSelectRocketOf`).
  - Harness: `launch crew [station=] [event=launch|sep|phasing|arrive|axis|hold400|hold200|hold20|dock|undock|depart|
    deorbit|entry|chutes|splash] [lead=60] [follow=1]`, `launch select kind=crew`, `launch state` -> `crew_vehicles[]`
    (phase, docked, range to station / port, the station-frame offset, the timeline in s from now), `launch check` ->
    `crew[]` / `crew_*` (separation and contact offsets, 0 m; largest acceleration and turn rate and where).
    `scripts/crew_rendezvous.satcmd`.
- **Visiting vehicles: fast tracks, cargo ships, the station's loss (2026-10-21).** A program's `crew` block is any
  vehicle that docks at a station (`Program::crew`, the LCrew machinery); new keys (launches.json; `launch::Program`):
  `cargo` (no people: Upcoming tags it CARGO, "(cargo)" in titles; "crewed" lists and the Missions station rows skip
  it), `return`: `lands` / `burns` (destroyed on re-entry: `burnsUp`), `ports`: the class of docking port it takes
  (the station model's `docking_ports[].use`, "crew" default / "cargo"; `SatModel::DockPort::use`), `share_crew_ports`,
  `stay_days`, `fast_dock_h` (windows, hours after lift-off), `heat_shield_m` (the entry glow's place below the port), `heat_shield_r_m` (its radius; a spaceplane's belly depth), `belly_half_len_m` (> 0: the glow along the belly).
  - **Fast-track rendezvous** (`candidate`, `crewFastD`, `CrewPhaseGeom` / `crewCatchup` shared with `buildCrew`): for a
    program with `fast_dock_h` the candidate tries the passes over its pad of the next 30 days (each a pure function of
    the batch) and takes the first whose station phase lets the far phases close in a window's time (the middle of the
    feasible stretch of D, for margin), preferring the first window (2-orbit) when it comes within 14 days of the first
    pass allowing any (`Cand::dockAfter`; such a launch is not moved by the pad rivals, `LBatch::crewFast`). Fast flights
    coast 15-25 min before the first burn and fly a 65-min approach (`kCrewApproachLegs[1]`; the ~1-day profile 2.7 h).
    Windows: Soyuz MS / Progress MS 3.0-3.7 h (2-orbit) or 5.6-6.4 h (4-orbit), Shenzhou 6.2-6.9 h, Tianzhou 2.4-3.6 /
    6.0-7.0 h; Dragon and Gaganyaan keep the ~1-day profile. Measured (mission-ops suite, 92 cached flights): fast
    dockings 3.3-6.8 h after lift-off. A nominal arrival may lie up to 31 days before its lift-off
    (`Program::earlyDays`): the pad rivals, Upcoming, `launch list` / `trace` / `crew` reach that far back.
  - **Cargo programs**: Progress resupply (`soyuz2_progress`, the Soyuz-2.1a without an escape tower, Baikonur; ISS +
    ROS, 3 a year each, Zvezda aft / ROS's aft port), Tianzhou resupply (`long_march_7`, Wenchang; 2 a year, Tianhe aft),
    Cargo Dragon resupply (`falcon9_dragon`; ISS, Haven-2, Axiom, Starlab, Orbital Reef: 2 a year each, 28-35 days
    docked; Haven-1 has one port and gets none). Progress and Tianzhou burn up: after the deorbit burn (aimed at the open
    ocean) the spacecraft becomes a re-entry fireball at the entry interface (`fbDrawReentry`, transient kind 9 =
    the cargo ship's re-entry, `transientContinue` hands the selection / Go to over). A station served by several
    programs has a plane per program; the extra (cargo) planes are appended after every other plane
    (`LPlane::extra`, `crewPlane`) so no other batch key moves. **Ports**: each program takes ports of its class
    (`k mod n`; with the era on only ports on modules there at docking, `since`); Cargo Dragon shares the crew ports,
    its schedule half a rotation after the crews' (`phase` = crew phase + 0.5) on the port the current crew is NOT on,
    undocking 3 days before the next crew needs it. `launch trace` -> docking `port_conflicts` (every other visit to the
    station at the same port overlapping its stay): 0 in the suite. Models: `tools/make_crew_models.py` (Progress,
    Tianzhou: in-orbit model only), `tools/make_rocket_models.py` (`soyuz2_progress`, `long_march_7`, folded arrays on
    the stage); the stations' Progress / Tianzhou greebles are gone (ISS, ROS, Tiangong carry a `"use": "cargo"` port).
  - **The station's loss** (`LCrew::tDeath`, from the roster's dead / end words = the event log; crewSync rebuilds the
    batches when they change): before arrival the rendezvous is called off (`abort` 1: it finishes the coast or
    transfer it is in, waits on that circle and comes home); on the approach it turns away from where it is onto the
    departure circle (2, `LCrewSeg::tCut`, `attFrom`); docked, a crew undocks in an emergency 90 s later and backs off
    fast (3, `CP_ESCAPE`), a cargo ship is destroyed with the station (4, gone at tDeath). Milestones, Missions >
    Ongoing ("The station is destroyed (date). <phase>") and the labels follow. No new launches to a lost station (as
    before). Harness: `launch crew craft=progress|tianzhou|cargo dragon|cargo|crew event=abort|reentry ...`,
    `scripts/station_loss.satcmd` (all four cases), `scripts/visiting_vehicles.satcmd`.
  - **Daylight review**: the entry's plasma point sits at the heat shield (`heat_shield_m` below the port along -Z,
    not at the model's origin) and gives way, as the capsule's mesh resolves, to the entry-plasma emitter (2026-10-21
    validation; see Starship's ship above): a shock layer 0.12 R + 0.1 m ahead of the shield (sigma 0.55 R across, 0.06 R
    thick; R = `heat_shield_r_m`: Dragon 1.85, Shenzhou 1.25, Soyuz 1.1, Gaganyaan 1.75, Cygnus 1.53, HTV-X 2.0), a hot
    wake behind the body and the 4-s path wake (centred 0.6 of it back, sigma 0.22: at 0.55 / 0.28 it reached ahead
    of the capsule), and one physical hull light 0.8 R ahead of the shield. It was a 3.5-m round Gaussian in the
    point's units (0.03 of its light): from 40 m a saturated disc several capsules wide. Canopies have
    12 alternating gores (Soyuz / Gaganyaan orange-white, Shenzhou / Dragon red-white; inside the same colours) and 16
    suspension lines to a confluence, a riser to the module (all render-only, `make_crew_models.py` `chutes`).
  - `launch check` -> `crew[]` gains `spacecraft`, `cargo`, `burns_up`, `fast_track`, `hours_to_dock`, `abort`,
    `burn_accel_max_mps2` (in space: burns, approach, deorbit), `key`; `crew_burn_accel_max_mps2` / `_in`,
    `crew_fast_track_count` / `_hours`, `crew_cargo_count`. Measured: burns <= 13 m/s^2 (the entry interface's
    hand-over), the entry <= 25 m/s^2, turns <= 1.5 deg/s. The mission-ops suite judges every cached visiting
    vehicle around each epoch (`VISIT_THRESHOLDS`, `launch_check_<epoch>.json`), not only the traced flights.
  - **Wings (2026-10-21)**: Progress's and Soyuz MS's fixed wings face ALONG the vehicle's axis (the cells seen face-on
    from in front / behind, as in photographs of an approaching Soyuz; the first cut had them edge-on to it: a thin
    line from behind); Tianzhou's and Shenzhou's turn about their span to the Sun (a child group "wings", joint
    `track` toward the Sun, rest pose facing +Z: `make_crew_models.py`, `_wing` marks). Photometric parts: the four
    models were selftested.
- **Berthing with the station's arm (2026-10-21; Canadarm2 on the ISS: Cygnus, HTV-X).** A program whose crew block has
  `"berth": true` (`Program::berth`, `grapple` [radius, z below the port] = the vehicle's grapple fixture, `capture_m`)
  does not dock: after the arc onto the port's axis (a NADIR port: the R-bar, from below) it closes to 250 m (hold
  20 min), 30 m (hold 15 min, go for capture), to the capture point `capture_m` (9 m) out, and free-drifts there; the
  arm grapples it 5 min later (`tGrapple`), moves it to 1.5 m (50 min) and onto the port (20 min, the CBM's bolts;
  `tDock`, phase `berthed`). At departure the arm takes it again (`armD0`), lifts it off the port (`tUndock`), moves it
  back to the release point, holds and opens (`tRelease`); it drifts off, a departure burn 2 min into the departure
  (`LCrew::tDepBurn`, now every visiting vehicle's), then the usual deorbit (both burn up). Legs `kBerthLegs`
  (buildCrew); new phases CP_TO_250 .. CP_ARM_INSTALL, CP_BERTHED, CP_UNBERTH, CP_RELEASE.
  - **The arm** (`launches.json` `"station_arms"`, `launch::StationArm`: the station by `match`, `model`, `base` +
    `base_normal` in the station body frame, `shoulder_m`, `boom_m`, `wrist_m`, the park `park_wrist` / `park_dir`, the
    elbow's `pole`): `LaunchImpl::stationArmPose(arm, t, visits)` is a pure function of t over the berthing visits to
    that station: the end effector's tip G and its approach direction follow a target — the park, a 0.6-m stand-off
    from the vehicle's fixture (its pose at the capture hold, berthed, or the release hold: fixed in the station's
    frame), or the fixture itself while it holds (`berthFixture`: crewPos + crewAttitude, in the station's frame) —
    blended by smootherstep (30 min park <-> stand-off, 2 min stand-off <-> fixture, an hour holding on after the
    bolts); the wrist point is the tip less `wrist_m` along it, and the two equal booms solve to it with the elbow
    toward `pole` (`armSolve`). The arm works with the first visit whose window holds t (`armA0` .. `armA3` on arrival,
    `armD0` .. `armD3` at departure). Drawn by `launchArmJobs` as three meshes in one reserved chunk below the ships'
    (`kArmMeshSlots`): `rocket_arm_canadarm2_boom` twice (shoulder -> elbow, elbow -> wrist) and `_wrist` (+Z toward
    the next joint, +Y the bending plane's normal; `make_crew_models.py write_arm`, lengths from launches.json); the
    shoulder base is the ISS model's render-only `gb_canadarm_pdgf` / `_shoulder` on Destiny's nadir side (the old
    static boom greebles are gone). The ISS's `docking_ports` gain `unity_nadir` (use `berth_unity`: Cygnus) and
    `harmony_nadir` (`berth_harmony`: HTV-X) — CBM rings 0.35 m proud.
  - **A station near a visiting vehicle is drawn by the CPU** (`launchArmJobs`: the arm's station whenever the arm is,
    and any station within ~3 px of size with a crew-list vehicle): the GPU lists no satellite in the Earth's shadow,
    so at night a docked or approaching vehicle (and the arm) floated beside a station that was not drawn.
    recordMeshScene skips a GPU candidate already among those jobs.
  - **Programs** (launches.json, appended: no other batch key moves): Cygnus resupply (`falcon9_cygnus`, SLC-40, every
    0.75 y, 60-100 days berthed at Unity nadir; NG-20 .. 23 flew on Falcon 9 while Antares 330 was built), HTV-X
    resupply (`h3_htvx`, H3-24W from Tanegashima, yearly, 120-170 days at Harmony nadir). Models: `rocket_capsule_cygnus`
    (UltraFlex discs) / `_htvx` (`make_crew_models.py`: CBM ring on top, grapple fixtures on both sides), the rockets
    (`make_rocket_models.py`: `capsule` "cygnus" / "htvx" under the fairing). Not built: Dream Chaser (Vulcan, a runway
    landing at KSC), Antares 330, the arm walking between grapple fixtures, the SSRMS's seven joints as separate parts,
    a free-flyer's capture dynamics (the vehicle holds still in the station's frame at the capture point).
  - **The info window's 3D view draws the arm too** (2026-10-21, `viewerArmParts`): its three parts posed from the
    station's root group in that view, as extra instances in the viewer pass (`SatMeshRenderer::recordViewer(...,
    extra, extraTypes, n)`, instance slots `kMaxInstances` .. + `kViewerExtra` 4, past every scene slot). The visiting
    vehicles at the ports are still not drawn there. Harness `launch state` -> `viewer_arm_parts`.
  - Harness: `launch crew craft=cygnus|htv event=capture_point|capture|berth|unberth|release|arm|arm_depart`;
    `launch state` -> `station_arms[]` (`with`, `attached`, `reach_short_m`, shoulder / elbow / wrist / tip in the
    station frame) and `arm_parts_drawn`; `launch check` -> `crew[].berth` (`capture_point_err_m`, `grip_tip_err_m`,
    `release_tip_err_m`, `arm_attached_err_m`, `arm_step_max_m_s`, `arm_turn_max_deg_s`, `arm_reach_short_m`) and the
    `berth_*` maxima; `launch trace` docking adds `hold250_range_m`, `hold30_range_m`, `capture_range_m`,
    `grip_tip_err_m`, `berthed_tip_err_m`, `release_tip_err_m` (mission_ops.py thresholds). Measured (4 cached
    visits): every tip error 0.000 m, capture-point error 0, arm joints <= 0.012 m/s, booms <= 0.12 deg/s, nothing out
    of reach; the vehicles' turns <= 0.9 deg/s. `scripts/berthing.satcmd`.
- **Dream Chaser: a runway landing (2026-10-21c).** Program "Dream Chaser resupply" (Sierra Space, CRS-2) on the new
  vehicle `vulcan_dreamchaser` (Vulcan VC4L: four solids, the long 5.4 m fairing; SLC-41), once a year, berthed by
  Canadarm2 at Harmony nadir with the berth machinery above (`grapple` [1.55, 2.2], `capture_m` 11, 45-75 days).
  **One port, two programs:** `"alternate_with": "HTV-X resupply"` (Program::alternateWith) puts its plane's phase half
  a cadence after HTV-X's (both 1 / yr), so they never share Harmony nadir. **Coming home** (`"return": "runway"`,
  `"runway": {lat, lon, heading_deg, length_m, elev_m}` = the KSC Shuttle Landing Facility 15/33, Program::runway):
  the Shooting Star cargo module is jettisoned 15 min before the entry (`tCm`; the `_cm` model is the spaceplane
  alone) and burns up 25 s behind it (a kind-9 fireball from the entry interface, the cargo ships' fbDrawReentry); the
  spaceplane flies the capsules' entry (nose / belly forward, the plasma glow) to 10 km, ending at 180 m/s over the
  ground along its track (`rwDv`, Earth-fixed: T rwDv (s^3 - s^2) added to the entry, zero at both ends; the capsules'
  entry ends drifting west with the Earth at ~120 m/s inertial), then glides
  (`crewRunwayEcef`, LCrew `runway`, `tGlide`, `Tg`, `tTD`, `tStop`): a cubic Hermite in the runway's tangent plane
  from the entry's end (its speed over the ground) to the touchdown point 450 m in along the runway the glide arrives
  from (at 95 m/s), and in height (10 km falling at 0 to its wheels, 2.6 m, sinking 1.5 m/s), then a rollout of
  2.4 km decelerating evenly to rest, 40 min on the runway. Phases CP_GLIDE ("gliding to the runway"),
  CP_FINAL_GLIDE (the last 90 s), CP_ROLLOUT, then CP_DOWN (inserted before CP_ABORT). **The deorbit is timed**
  (buildCrew, every 30 s from 3 h after the departure for six days, refined at 3 s near each pass) so the entry ends 60-160 km short of the runway
  heading for it, else the closest (measured 152-192 km at the three mission_ops epochs: a long subsonic glide from 10 km); the
  attitude follows the velocity (nose forward, belly down) and holds the rollout's at rest; through the entry it flies
  NOSE-HIGH (2026-10-21: 40 deg angle of attack about body X, eased in over the 10 min before the entry interface and
  out over the last 3 min before the glide, `crewAttitude`), its glow on the windward BELLY (`belly_half_len_m` 4.5:
  `heat_shield_m` 9.65 = the hull's middle along -Z, `heat_shield_r_m` 1.5 = the belly below the axis; a layer 1.8 x
  1.9 m across, two physical hull lights under it); it sat on the body axis. Milestones: cargo module
  jettisoned, entry interface, glide, final approach, touchdown, wheels stop. Models: `tools/make_crew_models.py`
  (`rocket_capsule_dreamchaser[_cm]`, sources: Sierra Space / Wikipedia; 9 m, 7 m span, Shooting Star 4.6 m, arrays,
  grapple fixtures, the CBM on top) and `make_rocket_models.py` (`rocket_vulcan_dreamchaser_*`, the spaceplane nose
  down under the fairing with folded wings: the sim's spacecraft convention puts the port on top). The wings must
  sweep with fin `sweep` > 0 (toward the nose): a negative sweep failed the occlusion self-test (p95 0.41). Harness
  `launch crew craft=dream event=jettison|glide|final|touchdown|stop` (and the berthing events), `launch trace` ->
  `docking.runway` {glide_start_jump_m, glide_km, glide_s, touchdown_speed_mps, touchdown_sink_mps,
  touchdown_off_centreline_m, touchdown_height_m, rollout_m, landing_heading_deg} (mission_ops.py thresholds);
  `scripts/dream_chaser.satcmd` (2035: launches stand down after the Sun's Fury). Measured: glide start 0 m, touchdown
  94.7 m/s, sink 0.75 m/s, on the centreline; mission_ops 0 FAIL / WARN. Open: no TAEM / heading-alignment circle (the glide is one curve), no
  drag chute, no landing gear deploying, the module's fireball follows the spaceplane's entry track 25 s late.
- **Missions window** (2026-10-19, `SatelliteSimMissions.cpp`; window id 8, `msChrome`; the rocket button
  `TimeMissionsBtn` on the right HUD panel, `pixel--missions.png`; key M, `KB_MISSIONS`; harness `ui open missions
  [tab=upcoming|ongoing|controls]`, `missions state|go|launch|catch|tab`). **ONE button per row, "Watch"** (2026-10-24,
  the user: "When a use wants to watch a lanch, they should just get one button that takes them to a rocket launching
  ... These '1 minute before touchdown' type event calls do a real diservice as they often confuse playtesters into
  teleporting to a place with nothing going on (yet)"; Go to / Select / Next > and the time-only Watch are gone): it
  lands on the event ALREADY HAPPENING, at 1x, the subject in frame and, when it moves, selected and tracked (the camera
  lock) once the transient list has it (`launchWantSelKey` + `launchWantTrack`, `selWantKind` + `selWantTrack`,
  selTransientUpdate; a want lapses ~2 min of sim time on). The toast says what you see ("Starship lifting off from
  ..."), never "in N s". Three tabs. **Upcoming**: the next 5 launches (vehicle, program / payload, pad, UTC, the Sun at
  the pad, a CREWED / TWILIGHT tag, the T- countdown); **Watch** = `launchWatchKey`: lift-off + `kWatchLiftoffS` (8 s:
  clear of the tower, the ground cloud rising) on the ground 5 km from the pad (`launchGoToKey(..., distM)`: the first
  land candidate), FOV 40, the stack tracked. Pinned whatever their order: the next twilight ("jellyfish": Sun 5-10 deg
  below the pad, searched day by day up to a week; it was 5-16, and below ~-10 the bells stay in the Earth's shadow —
  a Watch at -12 deg showed a black sky) and the next crewed launch; a twilight Watch is the first moment after T+150 s
  the upper stage is out of the Earth's shadow + 40 s (else `kWatchTwilightS` 300 s, at most SECO), from 40 km, FOV 60,
  the upper stage tracked (`state` -> `missions launch` `sunlit_from_s`). Last, always, THE SUN'S FURY card
  (`Pal::furyBg / furyBorder / furyText`): the CME's countdown, then "DAY n" and the cascade's counts; Watch = the storm
  night (the default bookmark's moment and weather, the aurora up; the "2 minutes before the arrival" button is gone:
  harness `missions go fury` keeps it). **Ongoing**: crew transfers in flight (ascent phase, then "docking in"), the
  crewed stations (altitude, next crew flight), satellite launches in flight, and per program the trains raising (count,
  mean progress, next arrival) and the retirees deorbiting (next re-entry); Watch = Go to the object (follow:
  `missionsOngoingAct(row, true)`), a station Go to, the retirees' Watch = one burning up
  (`launchGoToNextReentry(msg, watch=true)`: decay + 45 s, mid-glow, the breakup ~35 s on, from the side of its track
  under the clearer sky — a candidate whose both sides are > 0.35 cover on the CPU cloud map is skipped,
  `cloudCoverAtEcef` — FOV 35, the fireball tracked).
  **Kessler** (2026-10-21, replaced Controls; `SatelliteSimKesslerTracker.cpp`, `buildKesslerTrackerTab`): THE SUN'S FURY
  strip (countdown / DAY n / "computing"), big counts (collisions, catastrophic, pieces >= 1 m, 10-cm fragments made,
  dead, destroyed, re-entered, still working), a GLOBE (a CPU raster, `kTrN` 256^2, R8G8B8A8_SRGB, the UI's 4th
  external image — `kMaxExternalImages` is 6 now — uploaded by `kesslerTrackerRecordUpload` in recordCompute, re-rastered
  only when the counts publish, it is dragged or the Sun moves ~1 deg, at most 4 / s): seen from the equator, the shells
  drawn round it as a POLAR PLOT (radius = altitude x1.6, angle = latitude: the limb of an equatorial view is a meridian),
  blue = working satellites, heat = the chance a year a 10 m^2 satellite there is hit by a tracked object (density x
  10 km/s x 10 m^2 x a year; dead satellites + pieces >= 1 m, NOT the small-fragment grid, which lives on the worker);
  numbered markers for the 8 latest significant breakups (catastrophic collisions, explosions, ASAT, the player's) at
  their exaggerated place, a cyan dot for you; collisions per day (UIPlot, <= 120 bins); the breakups listed with Watch
  (`kesslerWatchEvent`: 3 s AFTER it at 1x, 80 km beside the orbit and 25 km BELOW it so the debris is against space;
  `kesslerWatchPieceTick`, after kesslerSync, then selects the first record of the breakup once the mirror has it — the
  pool scanned 400k slots a frame — re-places the eye beside where that piece REALLY is and tracks it. The re-placement is
  needed: the breakup's recorded place (`Event::pos`) was ~8100 km from its pieces for the newest catastrophic collision
  at day 0.6 (`missions state` -> `tracker.watch_piece_off_km`): an engine event's place is probably taken at its 6-h
  step's sample time while its `t` is spread through the step — not investigated; the dust clouds follow `Event::pos`). ALL of it comes from the counts walk
  (`kesslerCountsWalk`, ~150k records a frame while the tab reads `kesslerStateCached`): `KCounts` gained the alt x
  inclination histograms (`deb`, `work`), `colDay` / `catDay`, `recent[8]`, `working`, `reentered`, `explosions`;
  `kCountsLiveGen` bumps on each publish. The rows are cached
  (`missionsRefresh`: the next launches and the ongoing list every 0.5 s or on a 6-h sim jump; the pinned searches
  every 3 s or when sim time passes them) and rebuilt only while the window is open; `launchUpcoming` keeps the
  batches it lists alive (`touched`), so they are not rebuilt every ~600 frames. Strings handed to Clay are copied
  into `msText_` (a deque) — an action after the layout only marks the rows stale, never rebuilds them that frame.
  **Go to fixes the observer's altitude** (`obsHeightOffset = 0`, on the ground): `launchGoToNextWhen` and the launch
  re-entry Watch kept whatever height the eye had (a jump from orbit stayed in orbit).
  **The Sun's Fury card is pinned under the list on Upcoming** (2026-10-20: outside the `MsList` scroll view; at the
  default height it was the last card and needed scrolling). Ongoing launch rows have **Next >** (`missionsMilestone`):
  the batch's next milestone, carrying a selected / followed stage of it.
  **Launch Watch = T-5 s, NOT tracked (2026-10-26, issue 20261026_193645; the user: "We should not be auto tracking
  the vessel"; T-1 min was then too long a wait, cut to 5 s):** every launch Watch (`launchWatchKey`: Upcoming rows, the pinned twilight /
  crewed rows, Settings > Launches) lands `kWatchLeadS` (5 s) before lift-off at 1x, on the ground 5 km from the pad
  (12 km and FOV 60 for a twilight one), the camera on the stack, the STACK selected (`launchWantSelKey` = key << 3 ^ 1)
  and no camera lock. The twilight Watch's sunlit-stage scan is gone. A tracked Watch locked a playtester onto a stage they
  did not recognise.
- **QA sweep (2026-10-21):** the MISSION section's progress bar is progress through the milestone LIST (interpolated in
  time between the last passed and the next), so it agrees with "(n of N milestones)"; the selection panel's magnitude
  is the Info window's MAGNITUDE rule (the GROUND observer's: below the horizon, dark, or magnitude + extinction; also
  in follow mode); the Info 3D view draws a burning stage's flame (`GpuMeshFrame::flame0/1`, 688 B, drawn by
  `sat_mesh_marker.frag` at its own depth; the camera centre moves half the flame toward it) and nozzle glow, and no
  model for a destroyed / re-entered roster object (all group rotations zeroed; `buildViewerGoneLabel`); Missions >
  Ongoing puts the BUILD-OUT summary last, collapsed once complete (`msBuildOutOpen`); `buildResizableWindow`
  ellipsizes a title to the room left of its title-bar icons (`GenWinTitleExtras`, last frame's width). The Sun's
  lens-flare ghosts (`lensFlare` f2/f22/f23) are the CUBE of the old Lorentzian (its tail was a ~0.05 linear brown
  wash over half the frame from orbit, which hid a backlit station's earthlit side, ~0.003); the Sun's glare / corona
  lobes stay over a mesh while the Sun itself is unblocked (`veilGate`).
- **Mission milestones (2026-10-20, the user: "skip to the next mission milestone", "mission activity in the info
  screen"; `SatelliteSimLaunches.cpp` end, `missionBatchMilestones` / `missionSubjectOfIndex` / `missionJump`).** A
  launch's timeline is read from its batch's phase times: lift-off, max-Q (the largest 0.5 rho v^2 on the booster's
  track before MECO), MECO and staging (`vh.t1`), the fairing (`tFair`), SECO (`tL + t2`), deployment begins / last
  satellite released (`tD`, `deployInterval`), the deorbit burn (`tDeorbBurn`), entry (`tStageGone`), the ship's
  descent / flip / catch or splashdown (`tShipDesc / tShipFlip / tShipCatch`), docking for crew (`tA`). **The booster's
  recovery milestones are read off its `Track`**: every change of `burn` after staging (boostback, entry, landing, or
  any kind added later, named "Booster burn"), then `tEnd` (tower catch / pad / droneship / sea by `recovery` and
  `landed`) — so a new landing kind needs no code here. Trains: raising starts (min `sRaise`), arrival; a member adds its
  release and its own raise; retirees: deorbit (`tA`), re-entries (`ends`), a member its `tEnd`. Each milestone carries
  `who` (`kMsPayload | kMsBooster | kMsTrain | kMsRetiree`) and the SUBJECT's role filters them: the selection or the
  followed transient (`kind 1/2/4`: payload, or booster once a stage-0 transient is past `t1`), a launch record (train
  member / retiree by `launchSlotOwner`), a roster satellite (`missionRosterBatches`: the plane's batch (k, b) whose
  arrival put it in its slot and k + 1 that replaces it; `LaunchState::satPlane/satPos`, rebuilt with the planes;
  crew stations and Kessler-dead satellites have none). Facts rows (`missionFacts`): coming up, the record's status,
  the climb's progress and arrival, re-entry, a roster satellite's time in its slot, design life used, replacement.
  - **Skipping** (`missionJump`): to `t - lead` (10 s, or 0.45 of the gap to the previous milestone) of the next /
    previous / an indexed milestone, at 1x (pause kept), histories reset like any time jump. `missionAttachAt` keeps
    the selection and Go to on the subject's object at the new time — the rocket (`launchRocketTransient`, the camera
    keeps its offset in the object's frame via `followTakeTransient(c, 0, t)`), the booster (a stage-0 transient), a
    train member (the rocket before `tD`, its record in [tD, tA), its roster slot after), a retiree (the roster slot
    before `tA`, its record until `tEnd`), a roster satellite (the train member of the batch before it before that
    batch's `tA`). A launch record's slot is allocated by `launchUpdate` at the new time, so `missionAttachAfterJump()`
    runs right after it (before `followCheckTarget`) and re-points follow / the selection (30 frames, then the roster
    slot). UI: the Info window's MISSION section (`buildMissionTimeline`: mission, object, facts, progress bar,
    < Previous / Next: <label> >, one row per milestone with T+ from lift-off or "in" / "ago" for a roster satellite,
    UTC, Skip / Back; the last passed highlighted with `Pal::rowHighlight`), the follow HUD's `<` / `Next: ... >` chips,
    keys `]` / `[` (`KB_MILESTONE_NEXT` / `_PREV`, `missionJumpNow`: Go to's object, else the selection's), the Missions
    window's Ongoing **Next >**. Jumps from UI are applied AFTER the layout (they rewrite `followLabel`, which Clay
    still holds). Harness `missions milestone [list|next|prev|<i>] [row=]`; `scripts/mission_milestones.satcmd`,
    `scripts/mission_edge_cases.satcmd`.
  - **A selected / followed re-entry is listed even where it is not drawn** (`transientWanted`, used by
    `fbDrawReentry` and the retiree / upper-stage re-entry loops' 3000-km gates): behind the Earth, out of range, too
    faint or in daylight it is listed with flare 0 (picking skips it), so an expendable stage's selection carries on
    to its fireball (it was dropped: the fireball was never drawn from the pad).
- **The ship comes home to a tower; boosters are caught (2026-10-19, the user: "do Starships actually land?" — in the
  2036 setting SpaceX flies full reuse).** A returning stage's deorbit burn is TIMED for a catch (`planShipLanding`):
  the first moment within 72 h of deployment (30-s steps, refined at 2 s) at which its entry ends (`shipEntry` at
  te = `kShipEntryS` 480 s, 45 km) 120-450 km short of a Starship pad along its ground track and within 200 km across
  (score |cross| + 0.3 |along - 260 km|); only the phase of the burn moves the end, so it is a closed form per candidate.
  A pure function of the batch, like everything here. No pad in 72 h: the old burn time and a touchdown on the sea
  250 km past the entry's end (`landPad` -1; none measured in 1100+ batches). After the entry the descent
  (`shipDescentEcef`, Earth-fixed, in the landing pad's gnomonic plane + altitude, a cubic Hermite per axis):
  `tShipDesc` .. `tShipFlip` (T1 = 2 x the distance / the start speed, 150-1000 s: the start tangent stays under 3x the
  chord, no loop) belly first from 45 km to F = 1.1 km over the catch point, 400 m back along the approach, falling
  ~75 m/s; then the flip and landing burn (`BURN_LANDING`, T2 ~30 s) to the catch point at rest, 55 m above the pad's
  ground (`kShipCatchAboveGround`: the chopsticks at ~95 m hold it under its flaps), held in the tower `kCaughtHoldS`
  (30 min). Drawn by the ROCKET path (`rocketPos` stage 1 past `tShipDesc`: flame, mesh, plume parcels of the landing
  burn, its sound), the entry glow before it; the selection / follow continue through `transientContinue` (kind 2 ->
  kind 1 under the same key). Nose (`upperNoseRaw`): from the entry attitude to level along the ground track over
  90 s, upright from 2 s before the burn to 5 s into it. **The entry's start is continuous** (`entryOff`: the deorbit
  ellipse's position at the entry interface minus `shipEntry`'s, eased out over the entry's first 60 s; it jumped
  ~0.5 km). Boosters with `booster: tower`: the landing burn stops at `catch_m` (10) above the vehicle's `mount_m`
  (20; the stack stands on the mount), i.e. 30 m above the pad's ground, at its centre, held 30 min
  (`rocketPos` stage 0). The whole track stands on the pad's plateau + mount (`LBatch::padBase`, faded out over
  5-40 km downrange in `trackEcef`: a droneship deck stays at sea level). `launch check` (merged build, 1124 batches):
  ship entry start 7.9 m / 14 m/s (the 1-ms sampling step: was ~0.5 km), hand-over to the descent 3e-9 m / 7 m/s, the
  descent's largest acceleration 14 m/s^2, catch 0 m off centre at 55.0 m and 1.3 m/s, boosters 0 m off at 30.0 m;
  all 1124 ships caught (deorbit 20-62 h after deployment, descent 390-720 s from 250-330 km). Harness
  `launch landing [booster=1] [lead=] [dist_m=] [day=1]`; Settings > Launches > LAUNCHES "Watch a Starship catch"
  (`launchWatchCatch`, harness `missions catch`: 25 s before the catch, in the flip and landing burn, 2.5 km from the
  pad, the ship tracked; the other LAUNCHES buttons: the next launch / a twilight launch through `launchWatchKey`, a
  retiree burning up).
- **Launch pads on the ground (2026-10-19, the user: "launchpads and landing pads should have specialized graphics /
  play well with the city lights; exclusion zones near launch sites for the city grids").** One pad per distinct place
  among the catalog's sites (`buildPads`; sites within 150 m share one: 35 pads), its kind from the vehicles there (2
  Starship: catch tower; 1 Falcon: service structure + a landing zone; 0 other) or `pad_kind`, its layout's heading u
  the middle of its azimuths (`pad_heading_deg`) — turned so its back (-u: the access road, trench, tanks) faces the
  most land 1.5-3 km out when it faced water — and its ground height from the DEM (`demElevCpu`: the .r8 halves read
  four texels at a time, floored at 3 m). A Falcon pad's landing zone stands kLzDistM (2300 m) out on the driest
  land (`shoreDistCpu` on earth_water_sdf.r8 + DEM > 1 m), angle in `padAux.w`.
  - **GPU data**: CloudParams `padInfo` / `padQ` / `padU` / `padAux` (PAD_MAX 12, 960 -> 1552 B;
    `launchFillPadParams`, the nearest within 2500 km in the observer's ENU from its sea-level point, the q frame).
    In the UBO so every pass that needs them has them: the terrain, the sky pass and city_sprites.comp.
  - **Terrain** (`tdPadFlat`, terrain_detail.glsl, the nearest `padInfo.w` pads within 300 km): graded to the plateau
    within flatR (450 / 350 m), blending out to 2 flatR, no detail relief there (tdAmp0 x (1 - k): `gTdPadK` for the
    march, `TD_PAD_K(q)` in tdBegin). Water and land at the sea's level (marsh, sand bars, drawn as water) are left
    alone except the apron itself (`landK`, r < ~0.35 flatR): the first cut made every coastal pad a round island
    and the Starbase lagoon land (`probe`: hit 23 m with pads on, none off).
  - **Ground pattern** (`padGroundAt` / `padGroundOne`, sat_sky.frag, not SKY_LITE / SKY_ENV): graded patch (an
    irregular squarish outline, a 55% tint, not paint: a clean disc read as a blob from the air), concrete apron with
    10-m joints, Starship's steel deflector + soot and the tower base, other pads' flame trench, tank slabs, the access
    road (asphalt, sodium lamps every 40 m + their lit band) and the landing zone (concrete disc, painted ring and
    cross, its road), the tower's (lattice: half) and the mount's shadows. Exact box filters of the footprint
    (`cityBoxCover`, `solarPulseCover`, `padDisk` keeps a disc's area past the footprint): from orbit a pad is a pale
    speck. Only on dry ground (a terrain hit above 0.5 m): roads and zones crossed lagoons on land at 0 m. The pad's
    lamps and floodlit concrete are added to the night term. Debug view 28 (`debugview pads`).
  - **City exclusion** (`exclR` 3 / 3.2 km): the night map is cut to its base there (`nightColor` before the city
    detail texture and pattern), so no city grid, glitter, detail texture or farms (fW x (1 - excl)); city_sprites.comp
    skips its cells; beaches give way on the graded ground; close-up materials and micro relief too (`padCoreAt`).
  - **Meshes**: `data/satellite_models/pad_{starship,falcon,generic}.json` (`tools/make_pad_models.py`; body +Z up, +X
    the heading, origin the pad's centre on its plateau; one group: +Z zenith, +X "velocity" = the heading). Starship:
    the mount (table 20 m up on six legs), the 146-m lattice catch tower at v = `kPadTowerV` (-32 m) with its
    chopsticks at 95 m reaching over the mount, flood masts, the tank farm; Falcon: a red lattice FSS, three lightning
    masts, LOX sphere, RP-1 tank, water tower. Loaded with the rockets (`rocketTypes["pad_*"]`, loaded by the
    accuracy gate with them), drawn past ~1.4 px in 16 reserved launch slots (two chunks below the rockets':
    `kPadMeshSlots`, `launchMeshJobs`) as fixed states (the pad's ECI position at t, the Earth's rotation as velocity);
    not selectable (`rocketMeshSlotToTransient` -> -1).
  - **Lights at night** (`launchFillPadLights`, kFireBuf, within 150 km): flood clusters on the masts and tower
    (~-3 mag at 1 km), red beacons flashing 0.45 s in 1.5 (~-2), a landing zone's four floods; capped at flare 14 (just
    past the glare threshold; 2026-10-20: -4 / 25 drew a field of large stars from a pad's edge, issue 20261019_222938
    shot 1). The floodlit apron at night is 0.2 x its albedo (was 0.35: a blank white slab) x the floods' light
    (`padFlood`, sat_sky.frag, 2026-10-20: the masts' and the tower's floods of padLightsOf, each aimed at the centre in
    a cos^6 lobe, Lambert, 1 / (d^2 + 30^2), normalised to 1 at the centre, capped at 2.2): bright toward the mount and
    under the masts' aim, falling to the corners; each 10-m slab has its own shade (+-8%) and a weathered mottle at 7 m,
    both fading to their mean past the footprint.
  - Setting `launches.pad_graphics` (Settings > Launches > GRAPHICS "Launch site graphics"; independent of
    launches being on). Harness `launch pads`, `launch pad <site> [dist_m=] [az=] [h=]`. Cost (RTX 3070 Ti, 1600x900,
    Starbase from 900 m): sky pass within noise (8.09 vs 8.09 ms), scene depth +0.18 ms, CPU ~0; SKY_LITE SPV +288 B
    (the UBO's declaration; 213384 of 230400), sat_sky_minimal unchanged.
  - **Coasts around launch sites (2026-10-20, `data/launch_coasts.json` baked by `tools/make_pad_coasts.py`).** The 5-km
    water map cannot hold the narrow land and water around most coastal pads, and the DEM reads 0 m on that land: from
    the ground at Starbase the foreground read as sea (the land, at 0 m, took the day map's water-coloured texel) and
    the pads stood on islands of apron; at the Cape 39A sat in a dark-blue sea. Each group of coastal sites (13
    patches, sites within 6 km merged; inland sites none) gets a 48 x 48 signed distance grid over [-R, R]^2 (R 6-10 km:
    250-420 m cells) from the Natural Earth 10 m land + lakes (inside / outside by the whole rings, distance from their
    edges), every pad's graded disc (450 m Starship, 350 m others) forced to land, bytes of +-3000 m. CPU
    (`loadCoasts`, `launchFillPadParams`): the nearest 4 within 400 km into the UBO (`coastInfo / coastC / coastE /
    coastN / coastCol / coastG`, PAD block's tail, 1552 -> 11040 B; under the 16 KB uniform floor). GPU
    (terrain_detail.glsl `tdCoastPatch`, C1 bilinear, `tdCoastWm`): inside a patch the water map's shore distance is the
    patch's (blended back over its outer quarter), at the sea's level, tdShoreOffset's coves x0.3, and its land is lifted
    to at least `land_floor_m` (2.5 m) — so the march, the depth passes, the beaches (`shoreSignedDist`) and the sea all
    see one coast; sat_sky.frag draws its land in the patch's ground colour (`land_color`, a hand table in the tool:
    Boca Chica's sand flats, the Cape's scrub, the Louisiana marsh) where the day map is blue over its luminance, at
    least 75% (x a 420 / 90 m value noise). The CPU side mixes the patches into the pads' heading / landing-zone choice
    (`shoreDistAt`) and Go to's land test. Coastal lines are Natural Earth's (~1 km): the Cape's barrier at 39A is a
    ~1-km strip, Boca Chica's beach ~1.5 km east of the pad. Re-run the tool after editing the sites. Not in SKY_LITE /
    SKY_ENV. Off with "Launch site graphics".
  - The tanks' "pale" look at night was a stale environment probe (made by day, refreshed two faces at a time after
    3 s of wall time; see the env probes' sim-time jump rule), and probes inside the air now take the main observer's
    light-pollution dome for the Milky Way and zodiacal light (they used the pristine sky).
- **Booster recovery at sea: droneships and landing ships (2026-10-20, branch kessler).** Each launch decides its
  booster's recovery (`planRecovery`, a pure function of the batch, stored in `LBatch::rec` / `landKind`): a vehicle
  with `booster: droneship` and `rtls_max_kg` (Falcon 9, 7000 kg) flies back to its pad's LANDING ZONE when the launch's
  payload (the program's `sat_mass_kg` x payload or `payload_mass_kg`, x a hashed 0.9-1.1) is at most that: OneWeb and
  Reflect Orbital do, Starlink Gen1 (~9.3 t), Amazon Leo and Crew Dragon (12.5 t) do not. Otherwise it lands DOWNRANGE
  at `droneship_km` (Falcon 9 600, Crew Dragon 550, New Glenn 1000; 0 = where its arc comes down, the Chinese boosters
  ~390-430 km): on a ship when that point is at sea (`shoreDistCpu` > 3 km), picked by hash among the vehicle's
  `landing_ships` serving the site (Florida: A Shortfall of Gravitas / Just Read the Instructions; Vandenberg: Of Course
  I Still Love You; New Glenn: Jacklyn; Hainan: a recovery barge); on land (Jiuquan's inland arcs) on the ground there
  (`landBase` = the DEM less the track's touchdown height). `landKind` = kLandNone / Tower / Lz / Ship / Ground; harness
  `launch state` -> `boosters[]` (recovery, land_kind, touchdown_in_s, downrange, ship, dist_to_landing_m) — **the
  landing effects (dust, spray) key off `LaunchImpl::boosterLandingAt(L, B, t, E, tTouch)`**: the kind, the touchdown
  time and the point where the booster comes to rest at t (a ship's deck moves; for legs, the feet's surface).
  - **Tracks** (`launch::boosterTrack(vh, recovery, hLand)`): per vehicle its own (`L.booster`) and, for an RTLS-capable
    droneship vehicle, a return track (`L.boosterRtls`); `LaunchImpl::boosterTr(L, B)` picks this launch's. Downrange:
    a short retrograde TRIM burn (BURN_BOOSTBACK) secant-solved to touch down exactly at droneship_km (the old code aimed
    a boostback at 0.55 x the MECO speed that was never burned: every droneship booster came down ~410 km out); the
    entry burn runs 22 s, longer (up to 40 s) while still above 900 m/s. Falcon 9 / Crew Dragon / New Glenn stage
    faster (MECO 2.2 / 2.2 / 3.3 km/s: meco_downrange 120 / 115 / 225 km) so their arcs reach the ship. **A fast arrival
    (> 2.4 km/s descending, New Glenn's 1000-km shot ~3.3 km/s) starts its entry burn higher** (up to 100 km) and burns
    until its speed is one whose peak drag stays near 1.5 g (Allen-Eggers, v = sqrt(15 x 2e x 7.6 km / sin gamma)), up
    to 150 s (2026-10-21): it met the dense air at ~2 km/s and peaked at 92 m/s^2 (9.4 g), now ~45 (4.6 g). Slower
    arrivals are unchanged; mission_ops' `booster_recovery.max_proper_accel_m_s2` < 50 (FAIL).
  - **Landing legs** (2026-10-21): the track is the booster's aft end, and deployed legs stand `leg_m` (launches.json,
    the model's feet: tools/make_rocket_models.py's `legs`; Falcon 9 1.68, New Glenn 2.36, Zhuque-3 / LM-12 1.88,
    Tianlong-3 1.78, Pallas-1 1.68) below it: every recovery's touchdown height is + leg_m (`boosterTrack`), a ship ride
    home carries it (`rocketPos`), a ground landing's `landBase` includes it, and `boosterLandingAt`'s E is the surface
    (the feet). Trace / check `touchdown_above_m` measure the feet. A vehicle whose model's legs change must update
    leg_m (the feet sat 1.7-2.4 m below the deck).
  - **The landing burn is a constant deceleration that zeroes both components together at the deck** (Launches.cpp,
    every recovery, towers included): it starts below 8 km once that deceleration reaches 25 m/s^2 (a faster arrival
    burns harder, ~40-50 m/s^2 for 20-25 s), analytic from there. The suicide-burn profile it replaced touched down at
    ~40 m/s down and, for a droneship, ~360 m/s across. It ends at `kTouchV` 1.5 m/s straight down on the deck / catch
    height (the horizontal speed nulled), not at rest.
  - **The aerodynamic descent** (2026-10-20, issue 20261020_213739 "Rockets land tilted"): drag is a ballistic
    coefficient of 2200 kg/m^2 (terminal ~190 m/s at sea level; it was 280 m/s, half the real drag), and after the entry
    burn the grid fins / body lift steer the horizontal speed (up to `kGlideLD` 0.6 x the drag, time constant 3 s) to
    what reaches the landing point in the time left (+8 s; a ballistic droneship arc: to zero). So the booster comes
    over its target nearly vertical and the landing burn starts ~1.3-2.3 km up at ~260-340 m/s. Lean from the local
    vertical (`launch check` -> `booster_lean_deg_by_alt`, max over the batches, at 20 / 8 / 3 / 1 / 0.3 / 0.05 km and
    touchdown): ship 63 / 44 / 16 / 3.7 / 1.6 / 1.1 / 0.9 (mean at 1 km 0.7), landing zone 55 / 32 / 7.5 / 1.6 / 0.9 /
    0.7 / 0.7, ground 40 / 18 / 1.4 / 0.1 / 0 / 0 / 0, tower 24 / 7 / 0.2 / 0.2 / 0.2 / 0.2 / 0.2. Before (the same
    model replayed offline): a Falcon 9 to its ship 52 / 37 / 37 / 37 / 37 deg from 20 km to touchdown (the burn's
    thrust, 650 m/s across at 8 km), RTLS the same, New Glenn 49, Super Heavy 17, ballistic 25. The 20-km values are the
    entry burn (retrograde along the steep arc). Every landing moved later by the slower descent.
  - **Landing legs** (Falcon 9 / Dragon, New Glenn, LM-12, Zhuque-3, Tianlong-3, Pallas-1): `tools/make_rocket_models.py`
    draws them DEPLOYED (hinged a quarter of their length above the base, feet a little below the engine exits) with a
    `fold` back flat against the body; `transientLegs` (SatelliteSimLaunches.cpp, every rocket mesh write) sets the
    booster's unfolding progress: folded from lift-off, deployed over 2.5 s from 7 s before touchdown. The track's touchdown
    point is still the aft end on the deck, so the feet stand a little below it (the engines' exits are level with it).
  - **RTLS** to a Falcon pad's landing zone: the track lands on the pad's centre and over its last 150 s (by time; the last 15 s vertical) slides to the
    zone (`lzOff`, kLzDistM aside, its ground `LPad::lzElevM`), held there 30 min.
  - **The ship** (`shipEcef`): on station at the touchdown point from 8 h before lift-off (kShipOnStationS), the booster
    on its deck at tLand (the track ends `deck_m` above the sea there), 1.5 h later it steams home toward the launch site
    along the great circle at 4 m/s (eased in), drawn until 12 h after the landing (~150 km on). Its bow points home
    throughout. The booster rides the deck (`rocketPos` stage 0 past tEnd: `(kR + deckM) x shipEcef`; `boosterHoldS`),
    stays listed / selectable / followable (label "booster on <ship>'s deck, being secured / heading home"), and its
    coasting point sprite is dropped once landed (scenery: its mesh). Ship meshes: `ship_droneship` (ASDS: deck ~91 x 52
    m at 3.2 m, the housings and blast walls aft, the Octagrabber, rails, the landing target's ring and X) and
    `ship_jacklyn` (estimates: ~120 x 30 m, superstructure forward, landing deck aft at 6 m), written by
    `tools/make_ship_models.py` (body frame: +X the bow, origin the landing target at the waterline: `deck_m` in
    launches.json must equal the deck printed), loaded with the rockets (`ship_*` prefix, `rocketTypes`; the accuracy
    gate loads them), drawn past ~1.4 px in their own reserved chunk (`kShipMeshSlots` 8, below the stacks': the free
    chunks start 10 below the top; not selectable) by `launchShipJobs` (fixed state, the bow as the "velocity" axis).
  - **At night** (`launchFillShipLights`, `launchFillShipGlow`): flood clusters on the housings' masts (-2 at 1 km), red
    beacons, red / green side lights, a stern light (kFireBuf); the floodlit deck (-4) and a landed booster (-3.5) as
    EMISSIVE Gaussians in the dust list, only while the ship is under ~6-30 px (once its mesh resolves the deck and booster
    are lit by the mesh renderer's local lights, below); one engine light (`engineLightAdd`, 0.3 suns m^2 x night, lights
    the sea around it and low cloud; not the meshes). The landing burn's light is the rocket's own engine light.
  - **The swell** (2026-10-21, `shipSwell` / `shipFrame` / `shipDeckEcef`): heave, pitch and roll, each three sinusoids
    of hashed 6-14 s periods (up to ~0.9 m / 1 deg / 2 deg at sea state 1; Jacklyn x0.6) x the sea state at the ship
    (`SatelliteSim::shipSeaState`: the CPU mirror of sat_sky.frag's `oceanSeaState` with the storms from the CPU cloud
    map's coverage; `LBatch::seaK`, set eased by `launchShipJobs`). Everything on the ship rides `shipFrame`: the mesh
    (full attitude through `attFix`), the booster on the deck (position, and its axis = the deck's normal; it takes up the
    deck's motion over the landing burn's last 6 s), the lights. `launch ships` -> `sea_state`, `heave_m`, `pitch_deg`,
    `roll_deg`.
  - **The securing robot** (Octagrabber; Jacklyn's own): its own model `ship_robot` (tools/make_ship_models.py: the ship
    models no longer carry it) drawn in the ship mesh slots (key = the ship's ^ bit 63). Parked on the deck (droneship
    (-31.5, 0), Jacklyn (24, 0)), it drives out 75 s after touchdown at 0.6 m/s to stop with its front 4.6 m from the
    booster's axis, then swings its four clamp arms forward under the engine section (stowable parts, the unfolding
    progress over 15 s; `shipRobotAt`). `launch ships` -> `robot_deck_uv_m`, `robot_clamp_in_s`; `state` ->
    `ship_robot_meshes`.
  - Harness: `launch droneship [vehicle=] [day=1|night=1] [lead=60] [dist_m=1500] [az=60] [h=5]` (the next landing on a
    ship, 30 days ahead, 120 with a vehicle filter), `launch ships`; `launch check` -> `booster_recovery_counts`,
    `booster_landing_downrange_km`, `droneship_touchdown_*`, `lz_ground_touchdown_*`, `booster_ride_home_off_deck_m_max`,
    `droneship_examples`. `tools/harness/scripts/droneship.satcmd` (day from a boat and from 3 / 10 km, night, New Glenn).
    Measured (merged build, 14070 batches): 116 ship landings, 73 RTLS by payload, 85 ground, 183 expendable, 13613
    tower; touchdown on the deck 0 m off centre, 0 m above it; 1-s speed before touchdown <= 31 m/s (the end of a ~50
    m/s^2 burn); the booster on a steaming ship 0 m off its deck. Perf (RTX 3070 Ti, 1600x900, 1.5 km from the ship):
    GPU 13.8 ms by day, 12.5-21 ms at night (the cloud lit by the landing burn), CPU ~0 for the ships.
- **The deployment era: the constellations are BUILT UP from the late 2010s to 2036 (2026-10-20, branch kessler;
  setting `launches.deployment_era`, default on, Missions > Controls / Settings > Constellations "Deployment era
  (2019-2036)"; off = the 2036 roster at every date, the A/B).** Until then the full 2036 roster stood in the sky at any
  date. Now:
  - **Data** (data/launches.json): a program's `deployment` block(s) (one cohort each; `match` narrows a block to some of
    the program's constellations, e.g. Starlink Gen2 vs Gen3 Broadband): `start`, `complete`, `present_2026` (satellites
    up at `deployment_reference` 2026-10-20), `steepness` (8), `shell_stagger` (Starmind 1.5), `early_vehicles` +
    `early_until` (Starlink and DTC fly Falcon 9 until 2027-06: no stack, the satellites leave stowed from the dispenser
    point). `min_first_life` (0.25): a first-filled slot's first lattice refresh comes at least that x the design life
    later. Site `online` (+ `online_for: {vehicle: date}`, 39A's Starship tower 2027-03) and `fictional`; vehicle
    `online` (first flight). `roster_history`: groups without launches — stations' `born` dates (Tiangong 2021-04-29,
    Haven-1 2026-05-15, Axiom 2027-06, Haven-2 2028-06, ROS 2028-09, Bharatiya 2028-12, Starlab 2029-03, Orbital Reef
    2030-06: the first module's date; since 2026-10-21 the later modules follow their own "since" dates: Satellite Types > Station models), the HLS depots, the 2020 benchmark shells (V1.0
    2019-11 .. gone 2024-06, VisorSat 2020-08 .. 2025-01) and Space Junk (`count` over time: 9k 2000, 22k 2019, 30k at
    2026-10-20, 80k 2031, 300k 2036-11; members appear in a hashed order; the benchmark shells only on opt-in since
    2026-10-21, below). The ISS, Hubble and the GEO belt are always
    there (no entry). Dates are coarse real history before 2026, the scenario after.
  - **First fills** (buildPlanes, `LPlane::fillT`): per cohort every batch of its planes is put in fill order — shells
    one after another by altitude within each constellation (constellations side by side), overlapping by the stagger;
    within a shell batch 0 of every plane, then batch 1 (planes fill in parallel) — and the cumulative share of satellites
    mapped through the cohort's S-curve (`launch::SCurve`: a logistic over [start, complete], its midpoint solved so
    f(reference) = present / N) gives each batch's ARRIVAL. A first fill is refresh `kFillK` (-400000) in batch keys
    (`LBatch::first`): its "retirees" end as they start (nobody retires), labels say "first deployment". The lattice
    refreshes keep their times ((k + phase) L + b gap, unchanged), but one exists only from fillT[b] + min_first_life x L
    on (`batchExists`); `arrivalOf`, `forBatches` (lattice + first fills arriving in a window: the walk, the pad
    deconfliction's rivals, `launch list`, Upcoming, `deploy stats`) and `missionRosterBatches` (the first fill stands for
    the lattice refreshes it precedes) are the only places that know.
  - **rosterLife** (`rosterLifeEnsure`, SatelliteSimLaunches.cpp end): each roster satellite's born / gone second since
    the Kessler epoch (born = its batch's first-fill arrival, rounded up; history groups by their count crossings;
    INT32_MIN+1 / kNever = always), recomputed when the era or the roster changes (~100 ms at startup). **kesslerBuf's
    roster words are 4 per satellite now**: dead, end (2i, 2i+1), then the born / gone region from word 2N
    (`kLaunchWord0` = round4(4N)); sat_orbit.comp hides a roster satellite outside [born, gone) (N = poolBase -
    LAUNCH_CAP); `rosterLifeUpload` writes it (kesslerUploadAllSync and on a change). `rosterFateAt` returns 4 "not
    launched yet" / 5 "retired" (selection, Explode, `kessler breakup`, the Info window and the Missions stations refuse
    or say so). The Kessler world takes it (`World::rosterBorn/Gone`, `rosterPresent`): no collisions, no storm kill for a
    satellite that is not there; the cache hash includes it (kCacheVersion 15). A crew flight to a station not there yet
    is invalid ("station not there yet").
  - **Sites and vehicles come online**: candidate() and the deconfliction skip a site / vehicle before its date
    (`onlineAt`, true when the era is off), the ship catch picks only online Starship pads, and a pad not built yet is
    not drawn (ground pattern, mesh, lights). **The deconfliction's alternative pads start after the candidate's pad in
    the vehicle's site list** (they started at its head: every launch pushed off a pad landed on the first few, 20 a day
    on Louisiana A-1).
  - **The pad expansion** (Starship: 15 -> 42 sites): Starbase Pad 2 (2026-06), Louisiana A-E 2029-06 .. 2031-09 (were
    always there), **fictional**: Louisiana F-J (10 pads east of E, 2030-06 .. 2031-12), 6 Gulf sea platforms 60-90 km
    south of Louisiana, 3 "Starbase South" platforms off Boca Chica (the 26-28.5 deg shells only reach from below
    ~28.5 N), 4 Pacific polar platforms off Point Conception, 2 Cape Atlantic platforms (2030-01 .. 2031-12); KSC LC-49
    (the real proposal, 2029-06), SLC-37 2027-09, SLC-6 2028-06, SLC-4E for Starship 2029-06. Sea platforms are drawn as
    pads on the water (the graded apron is a small island; no coast patch: they are > 10 km offshore).
    tools/make_pad_coasts.py: a group spans at most 8 km (the 20 Louisiana pads are 4 patches) and every Starship site
    gets the 450-m disc.
  - **Numbers** (harness `deploy stats`, 2026-10-20 .. 2036-11-22, from the schedule; invalid batches still counted):
    196,632 launches — Starmind 188,529 (123,584 first fills at 8 a ship + 64,945 refreshes), Starlink 2,975, Guowang
    2,138, Qianfan 1,037, DTC 1,019, Gen1 290, Reflect 231, Amazon Leo 228, crew ~175, OneWeb 27. Busiest day 107
    launches (2032-12-15), Starship 104 (2032-08-27); quarterly means ~99 a day mid-2032, 63 in 2036 (the old 2036 sky:
    ~54). Same-pad pairs within 90 min per week (`launch padstats hours=168`): 2032-09 at ~99 a day 398 (29 pads,
    alternatives from the list head) -> 182 (rotated alternatives) -> 89 (+13 platforms, earlier dates; busiest pad-day
    20 -> 9); 2034 39 at 74 a day; 2036 12 at 52 a day (era off: 12 at 54).
  - **2036 vs before**: the roster at 2036-11-22 is the same but for the retired benchmark shells (1,377,341 of
    1,380,509 there); the lattice refreshes are unchanged where they exist, but a plane first filled less than 0.25 L
    before a lattice refresh skips it (batches live at the default start 5,187 vs 5,363 era off; trains 16,422 vs 16,782).
    The schedule moved anyway: adding sites to Starship's list re-hashes every launch's pad, so the default bookmarks
    "Twilight Starship Launch" (now 2036-11-22 12:10 UTC, T+290 s from Louisiana E-1, Sun -7.7 deg; the script searches
    from 11:50)
    and "Starmind Deployment" were re-shot (tools/harness/scripts/default_bookmarks.satcmd). `launch look` prefers the
    launch `launch next` went to (with dozens of pads the nearest stage was often another rocket on its pad).
  - **Missions window**: Ongoing opens with BUILD-OUT (DEPLOYMENT ERA) — per cohort deployed / target, launches a day
    now (first fills from the curve's slope + refreshes), complete date (`deployRows`); Upcoming tags first fills
    "BUILD-OUT"; a station not there yet reads "Not launched yet (date)". Harness `deploy`, `deploy stats`,
    `launch padstats` (docs/HARNESS.md); `missions state` -> `build_out`.
  - **Perf** (RTX 3070 Ti, 1600x900): launches CPU 0.97 ms at the 2036 default start (1.04 era off); 2.17 ms on the Gulf
    coast at a 2032 twilight (plumes 1.75 ms with ~1,700 candidates). 2032-09: ~9,700 live batches, 28k train satellites.
  - **Pads by date (2026-10-21):** a pad's kind has a history (`LPad::kindHist`, `padKindAt(pad, t, era)`,
    `padAtTime` = a copy with that kind's exclusion / graded radii): a site's `pad_history` [[date, kind], ...] (kind -1 =
    nothing there) or, without one, a generic pad from its `online` date raised to each of its vehicles' kinds from their
    `online_for` dates there; the pad is the largest of its sites' kinds. 39A: the Shuttle-era pad (generic) from 1966,
    Falcon 2017-02, its Starship tower 2027-03; SLC-37 a Delta IV pad 2002-11 .. 2025-06, nothing, Starship 2027-09;
    SLC-6 Delta IV 1985 .. 2023-06, Starship 2028-06; LC-36 built for New Glenn from 2021; Starbase's tower from
    2021-07; every other pad from its `online` date (Louisiana, the sea platforms: nothing before). Everything drawn
    follows it: the mesh (`launchMeshJobs`, slots keyed by pad AND kind), the ground pattern, the graded terrain and the
    city exclusion (`launchFillPadParams`: padU.w = the kind then, padQ.w / padAux.z its radii; a Falcon pad's landing zone
    only while it is a Falcon pad), the lights, `launch pad`; and the ship catch picks a pad with a catch tower at the
    catch (`padKindAt(...) == 2`: no catches at 39A before 2027-03), the lift-off deluge a Starship mount at lift-off.
    `LPad::kind` stays the 2036 pad (the era off). Harness `launch pads` -> `kind_now`, `history`; `deploy` counts pads
    by kind now. `tools/harness/scripts/era_build_up.satcmd`.
  - **The 2020 benchmark shells are left out of the era's sky (2026-10-21, a decision):** V1.0 / VisorSat (roster_history
    `"benchmark": true`) stand for satellites Starlink Gen1's own first fills already fly from 2019-05 (they were counted
    twice in 2019-2024). With the era on they are never born unless the player opts in — Settings > Constellations >
    BENCHMARK SHELLS (2020) "In the deployment era" (`launches.era_benchmark_shells`, default off; in rosterLifeKey). With
    the era off they are always there, as before; the benchmarks and the accuracy gate read the models directly. The
    alternative — Gen1's early slots flying the V1.0 / VisorSat models in their eras — needs a satellite's TYPE to change
    with time (a roster record's typeIdx is fixed: a per-satellite type word on the GPU), so it was not taken.
    kCacheVersion 16.
  - Open: stations' module launches (the build-up is dated parts, not flights); the dated parts' occluders in the opt-in
    part occlusion; OneWeb / Amazon Leo stacks (Launches > Payload fairings and stacks).
- **Mission-operations suite and launch benchmark (2026-10-20, `tools/harness/mission_ops.py`, `launch_bench.py`;
  docs/HARNESS.md "Mission operations" / "Launch benchmark").** Harness `launch trace` (`launchTraceJson`,
  `launchTraceBatchJson`, end of SatelliteSimLaunches.cpp) finds the next launch of every (vehicle, program) pair after a
  date — the batches' CANDIDATES first (vehicle, time; cheap), then building those of the pair in time order until one
  flies — and measures it phase by phase from the launch model itself (rocketPos, memberState, stageNose, the tracks;
  no frames): pad, climb, max-Q, staging, booster recovery, fairing, insertion (parking altitude, circular speed,
  flight-path angle, inclination and angle to the target plane), coast, deployment (release error vs the dispenser /
  stack slot, separation speed, member gaps), raising (proper acceleration, spread, monotonic climb), arrival and
  retiree start (position and velocity vs the slot, at the SAME instant), upper-stage deorbit / entry / a ship's catch,
  docking. `launch formation` checks a formation train (Starmind) from release to arrival; `launch goto` jumps to a
  launch's T+t. The thresholds are one Python table (`mission_ops.THRESHOLDS`, PASS / FAIL / WARN for the landing burn's
  physics, which another agent owns; the crew `docking` phase checks the spacecraft's flight). `selftest.py -k mission_ops` runs it at the default start.
  **What it found and what was fixed:**
  - **Staging**: `boosterTrack` never stored its MECO sample (fly() records from t1 + dt), so every post-MECO sample sat
    one index early: the booster's whole flight after staging ran 0.5 s ahead and the stack's last half second moved
    at twice its speed (an ~800 m jump at separation; velocity "jump" 1,200-3,450 m/s). The sample is pushed now.
  - **SECO**: the upper track's x is the arc ALONG THE GROUND (rocketPos turns x / kR into the inertial plane) but its
    rate was the inertial speed, so the stage ran 5-13% fast and dropped 250-940 m/s in one step into its parking orbit.
    Its rate now eases from the ascent's ground rate to vOrb x kR / (kR + h). This moves where every upper stage is
    after MECO (the parking orbit's phase, deployment): re-check bookmarks that frame a stage in orbit.
  - **Fairings** separated at MECO + 30 s (~60-100 km) on every vehicle: the 110-km search ran before setDeorbit, while
    tStageGone was still 0 and rocketPos returned nothing for the upper stage. It runs after the deorbit plan now.
  - **Trains' arrival**: a slot's mean motion is the roster's (J2), not Kepler's at its radius, and the climb ended at
    Kepler's rate (`launch::rampPhase` takes a rate factor f0 -> f1 now: the climb ends at the slot's rate, a retiree
    starts at it). The phasing residual (`LMember::eps`) is spread as a smoothstep, not a constant drift that stopped dead
    at the arrival (Starlink direct-to-cell's short raise: an 18 m/s jump onto the slot).
  - **Formations raise as one group**: every member of a cluster takes its leader's schedule (wait, ramp, residual:
    `buildBatch`, `gPerMemberRaise` = the old behaviour for `launch formation before=1`). Each member had its own hashed
    ramp, so a Starmind ring's eight spread up to ~10,000 km along the orbit during the climb and met only at the
    arrival, each from its own side. Now the pez string (45 s apart) eases into the ring over the release offset's two
    days (spread ~3 km at most) and the eight climb as a ring (shape within metres, `ring_shape_err_after_2d_m_max`).
  - **Trains left too close**: dispenser launches released a satellite every 2 s at ~0.45 m/s +- 0.12 (stacks 2-4 s at
    0.3 m/s): ~1 m apart for minutes, bodies overlapping. launches.json: `deploy_interval_s` 30 and `sep_spread_mps` 0.04
    on every dispenser vehicle (30 s apart, ~13 m at release; 12 s still met at ~1 m from the Clohessy-Wiltshire offsets),
    and 30 s for the Starlink Gen1 and Qianfan stacks.
  - **Milestones** (`missionBatchMilestones`) used the vehicle's own booster track and recovery for max-Q, the burns and
    the landing label: an RTLS Falcon 9 listed a trim burn and "lands on the droneship"; a ground landing likewise. They
    use this launch's track (`boosterTr`) and `B.landKind` now.
  **Still open (reported, not fixed):** New Glenn's booster peaks at ~92 m/s^2 after staging (the landing physics; WARN); a deorbit burn blended by smoothstep
  peaks at ~4x dv / burn (up to ~90 m/s^2 for the 800-km Qianfan stages: no jump, but violent); late close passes
  (~1 m, about an orbit after release) between train members, from the Clohessy-Wiltshire offsets taken about each
  member's own plane; Long March 12 flies no program. (Crew docking was SKIP until the crew spacecraft landed the same
  day: its `docking` phase now checks LCrew — separation and contact offsets, the 400 / 200 / 20 m holds, docked
  after contact and mid-stay, undocking, jumps and turn rates: 13 crew flights PASS over the three epochs.)
  **Measured:** (2026-10-20, RTX 3070 Ti shared with other agents' runs, so GPU numbers are noisy) `mission_ops.py run`: 69 launches over
    three epochs (27 pairs each), 947 phase checks PASS, 2 FAIL (Falcon 9 Starlink Gen1 and OneWeb at 2026-11-05: a pair within ~1-1.6 m
    in the first 10 min, from the random spread), 3 WARN (New Glenn's booster peaks at 92 m/s^2 of proper acceleration
    after staging; before the 7b729c7 landing rework 42: every recovered booster tilted 17-50 deg landing), 13 SKIP
    (docking); Starmind's ring shape error from day 2 ~0 m with the shared schedule vs 10,000 km before. `launch_bench.py`
    (harness_runs/launch_bench/baseline.json, launches on - off): the Twilight Starship bookmark +2.8 ms GPU (cloud_march
    +2.3: plumes) and 1.9 ms CPU; at 1 h/s 6.3 ms CPU (plume builds 4.6 ms); at 1 d/s 3.0 ms CPU (the window walk 0.9,
    record writes 1.0); a 2032 Gulf dusk +21.6 ms GPU (cloud_march +14.4, scene_depth +5.8) and 4.7 ms CPU; following a
    night Starship from 300 m +6.1 ms; a droneship landing +22.9 ms GPU, almost all cloud_march (the landing cloud /
    plume Gaussians).
  The "Starmind Deployment" default bookmark was re-shot (the SECO fix moved the ship ~90 km along its orbit;
  harness_runs/bm9/bm9.satcmd = section 9 of default_bookmarks.satcmd); `launch_bench.py` captures both launch bookmarks
  at its end so a moved stage shows.
- Not built: the booster's legs deploying,
  the knife-edge roll SpaceX uses to dim raising trains, J2 drift into the target plane
  (trains launch into it), Dream Chaser's berthing and runway landing, the stations' own phasing manoeuvres
  (a fast track waits for the right pass instead), hole-punching through cloud decks, sonic booms.
